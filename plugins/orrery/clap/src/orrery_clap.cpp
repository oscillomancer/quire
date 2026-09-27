#include "orrery/clap_plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "orrery/engine.h"
#include "orrery/patch.h"
#include "supple/base/spsc.h"
#include "supple/clap/plugin_support.h"
#include "supple/clap/semantic_model.h"

namespace supple::orrery {

namespace {

constexpr std::uint32_t kStateMagic = 0x3152'4F51;  // "QOR1"
constexpr std::size_t kMaxEvents = 4096;

constexpr const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                     CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.suppleaudio.orrery",
    "Orrery",
    "Supple Audio",
    "",
    "",
    "",
    "0.1.0",
    "Constrained-modular synthesizer programmed through a text patch.",
    kFeatures};

constexpr std::string_view kDocs =
    "Orrery is a constrained-modular synthesizer. A patch (supple.orrery-patch/1.0) "
    "chooses which slots exist — oscillators osc1..osc4 (va, wavetable, fm, noise, sub), "
    "filters filter1/filter2 (ladder, svf, comb, formant; serial or parallel), drive, "
    "envelopes amp/env2..env4, lfos lfo1..lfo4, steps step1/step2, eight macros and up to "
    "five post-FX in fx.chain — and routes modulation as \"<source>><destination>\": "
    "{\"amount\": ...} in the destination's unit (oct for frequencies, st for pitch, dB for "
    "levels, x for oscN.fm, % otherwise). Values are CLAP parameters keyed by patch path "
    "(osc1.level, filter1.cutoff, amp.attack); structure lives only in the patch. Macros "
    "are the automation surface for modulation.";

struct Plugin;

// The patch as seen by the semantic extension: structure and envelope on the
// main thread, values in the ParamStore (shared with the audio thread).
class Model final : public clapx::SemanticModel {
  public:
    explicit Model(Plugin& p) : p_(p) {}
    params::Table table() const override { return orrery::table(); }
    std::string patchTypeId() const override { return type_.schemaId(); }
    json::Value patchSchema() const override { return type_.jsonSchema(); }
    std::string patchJson(bool changedOnly) const override {
        return type_.serialize(current(),
                               changedOnly ? patch::Detail::Changed : patch::Detail::Full);
    }
    Status apply(std::uint32_t mode, const json::Value& document) override;
    std::string describe(bool changedOnly) const override {
        return orrery::describe(current(), changedOnly);
    }
    bool paramActive(std::size_t index) const override {
        return orrery::paramActive(structure, index);
    }

    Patch current() const;
    void install(const Patch& p);  // store + engine
    const PatchType& type() const { return type_; }

    Structure structure;
    patch::Envelope meta;

  private:
    Plugin& p_;
    PatchType type_;
};

// An editor edit on its way to the audio thread and the host.
struct EditItem {
    enum Kind : std::uint8_t { Begin, Value, End };
    std::uint16_t index = 0;
    Kind kind = Value;
    double value = 0.0;  // physical
};
using EditRing = SpscRing<EditItem, 4096>;

struct Plugin final : EditorBridge {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_supple_semantic_params_t* hostSemantic = nullptr;
    clapx::ParamStore store{orrery::table()};
    Engine engine;
    Model model{*this};
    clapx::SemanticAdapter adapter{
        model,
        {kDescriptor.id, kDescriptor.name, kDescriptor.version, std::string(kDocs), kStateMagic}};
    std::vector<Event> events;              // audio thread; reserved at activation
    std::vector<float> scratchL, scratchR;  // 64-bit or mono hosts: render here, then convert
    bool mono = false;                      // the selected audio-ports config (inactive only)
    std::array<std::int16_t, 2600> indexOfId{};
    std::array<std::atomic<int>, kOscCount> oscTypes;  // -1 for absent slots
    std::unique_ptr<std::atomic<bool>[]> activeParams =
        std::make_unique<std::atomic<bool>[]>(paramCount());
    bool active = false;
    std::atomic<bool> processing{false};

    // Editor (main thread unless noted).
    const ClapExtensions* ext = nullptr;
    void* extData = nullptr;
    EditRing edits;                       // main → audio thread (or params.flush)
    std::atomic<bool> retryFlush{false};  // audio thread asks main to request_flush
    std::uint64_t structureRevision = 0;
    std::int32_t gestureIndex = -1;
    bool gestureQueued = false;
    std::atomic<bool> telemetryOn{false};
    std::atomic<float> peakL{0.0f}, peakR{0.0f};  // written by the audio thread
    std::atomic<int> voices{0};

    Plugin() {
        indexOfId.fill(-1);
        const auto t = orrery::table();
        for (std::size_t i = 0; i < t.size(); ++i)
            if (t[i].id < indexOfId.size()) indexOfId[t[i].id] = static_cast<std::int16_t>(i);
        const Patch init = model.type().defaults();
        model.structure = init.body.structure;
        model.meta = init.meta;
        for (int o = 0; o < kOscCount; ++o)
            oscTypes[o].store(
                init.body.structure.osc[o] ? static_cast<int>(*init.body.structure.osc[o]) : -1);
        for (std::size_t i = 0; i < paramCount(); ++i)
            activeParams[i].store(orrery::paramActive(init.body.structure, i));
        store.assign(patch::FlatBody{init.body.values});
    }

    // A fixed CLAP table exposes the union of each slot's choices. Host
    // automation can therefore send a wave the current oscillator type does
    // not support. Keep the stored value in the patch codec's valid domain.
    double patchValue(std::size_t index, double physical) const SUPPLE_RT {
        const auto loc = locate(index);
        double value = params::clampPhysical(orrery::table()[index], physical);
        if (slotInfo(loc.slot).block == Block::Osc && loc.field == static_cast<int>(OscF::wave)) {
            const int t = oscTypes[loc.slot - kOsc1].load(std::memory_order_relaxed);
            if (t >= 0 && !waveApplies(static_cast<OscType>(t), static_cast<Wave>(value)))
                value = static_cast<double>(defaultWave(static_cast<OscType>(t)));
        }
        if (loc.slot == kPhaser && loc.field == static_cast<int>(PhaserF::stages)) {
            const int stages = static_cast<int>(value);
            if (stages % 2 != 0) value = static_cast<double>(stages + 1);
        }
        return value;
    }

    bool setHostClap(clap_id id, double clapValue) SUPPLE_RT {
        if (id >= indexOfId.size() || indexOfId[id] < 0) return false;
        const auto index = static_cast<std::size_t>(indexOfId[id]);
        const auto& spec = orrery::table()[index];
        if (spec.flags & params::kReadonly) return false;
        if (!activeParams[index].load(std::memory_order_relaxed)) return false;
        store.set(index, patchValue(index, params::fromClap(spec, clapValue)));
        return true;
    }

    bool applyHostEvents(const clap_input_events_t* in) SUPPLE_RT {
        bool changed = false;
        const std::uint32_t n = in ? in->size(in) : 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t* h = in->get(in, i);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
                continue;
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            changed = setHostClap(e->param_id, e->value) || changed;
        }
        return changed;
    }

    // CLAP events → engine events (audio thread, no allocation: capacity reserved).
    void translate(const clap_input_events_t* in) SUPPLE_RT {
        const std::uint32_t n = in ? in->size(in) : 0;
        for (std::uint32_t i = 0; i < n && events.size() < kMaxEvents; ++i) {
            const clap_event_header_t* h = in->get(in, i);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            Event e;
            e.time = h->time;
            switch (h->type) {
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE: {
                    const auto* ne = reinterpret_cast<const clap_event_note_t*>(h);
                    e.type = h->type == CLAP_EVENT_NOTE_ON    ? Event::Type::NoteOn
                             : h->type == CLAP_EVENT_NOTE_OFF ? Event::Type::NoteOff
                                                              : Event::Type::Choke;
                    e.key = ne->key;
                    e.channel = ne->channel;
                    e.noteId = ne->note_id;
                    e.value = ne->velocity;
                    events.push_back(e);
                    break;
                }
                case CLAP_EVENT_PARAM_VALUE: {
                    const auto* pe = reinterpret_cast<const clap_event_param_value_t*>(h);
                    if (pe->param_id >= indexOfId.size() || indexOfId[pe->param_id] < 0) break;
                    const auto index = static_cast<std::size_t>(indexOfId[pe->param_id]);
                    setHostClap(pe->param_id, pe->value);
                    e.type = Event::Type::Param;
                    e.index = static_cast<std::uint32_t>(index);
                    e.value = store.get(index);
                    events.push_back(e);
                    break;
                }
                case CLAP_EVENT_MIDI: {
                    const auto* me = reinterpret_cast<const clap_event_midi_t*>(h);
                    const std::uint8_t status = me->data[0] & 0xF0;
                    e.channel = static_cast<std::int16_t>(me->data[0] & 0x0F);
                    const std::uint8_t d1 = me->data[1], d2 = me->data[2];
                    switch (status) {
                        case 0x90:
                        case 0x80:
                            e.type = status == 0x90 && d2 > 0 ? Event::Type::NoteOn
                                                              : Event::Type::NoteOff;
                            e.key = d1;
                            e.value = d2 / 127.0;
                            break;
                        case 0xB0:
                            e.type = Event::Type::Controller;
                            e.index = d1;
                            e.value = d2 / 127.0;
                            break;
                        case 0xE0:
                            e.type = Event::Type::Pitchbend;
                            e.value = (static_cast<int>(d1 | (d2 << 7)) - 8192) / 8192.0;
                            break;
                        case 0xD0:
                            e.type = Event::Type::Pressure;
                            e.value = d1 / 127.0;
                            break;
                        case 0xA0:
                            e.type = Event::Type::PolyPressure;
                            e.key = d1;
                            e.value = d2 / 127.0;
                            break;
                        default:
                            continue;
                    }
                    events.push_back(e);
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Editor edits → host events (and engine events when `toEngine`). Stops
    // at the first event the host refuses and asks the main thread to request
    // another flush. Returns whether any value edit was drained.
    bool drainEdits(const clap_output_events_t* out, bool toEngine) SUPPLE_RT {
        bool values = false;
        EditItem item;
        const auto t = orrery::table();
        while (edits.peek(item)) {
            const params::ParamSpec& spec = t[item.index];
            bool pushed = false;
            if (out) {
                if (item.kind == EditItem::Value) {
                    clap_event_param_value_t ev{};
                    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE,
                                 0};
                    ev.param_id = spec.id;
                    ev.note_id = -1;
                    ev.port_index = -1;
                    ev.channel = -1;
                    ev.key = -1;
                    ev.value = params::toClap(spec, item.value);
                    pushed = out->try_push(out, &ev.header);
                } else {
                    clap_event_param_gesture_t ev{};
                    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID,
                                 static_cast<uint16_t>(item.kind == EditItem::Begin
                                                           ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                           : CLAP_EVENT_PARAM_GESTURE_END),
                                 0};
                    ev.param_id = spec.id;
                    pushed = out->try_push(out, &ev.header);
                }
            }
            if (!pushed) {
                if (!retryFlush.exchange(true) && host->request_callback)
                    host->request_callback(host);
                break;
            }
            edits.consume();
            if (item.kind != EditItem::Value) continue;
            values = true;
            if (toEngine && events.size() < kMaxEvents) {
                Event e;
                e.type = Event::Type::Param;
                e.index = item.index;
                e.value = item.value;
                events.push_back(e);
            }
        }
        return values;
    }

    void requestFlush() {
        if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
    }

    // --- EditorBridge ------------------------------------------------------------------

    const Structure& structure() const override { return model.structure; }
    double value(std::size_t index) const override { return store.get(index); }
    std::uint64_t revision() const override { return structureRevision + store.generation(); }

    void beginEdit(std::size_t index) override {
        gestureIndex = static_cast<std::int32_t>(index);
        // A gesture starts only with room for its final value and end.
        gestureQueued = edits.free() >= 3 &&
                        edits.push({static_cast<std::uint16_t>(index), EditItem::Begin, 0.0});
        requestFlush();
    }

    void edit(std::size_t index, double physical) override {
        if (index >= store.size() || (orrery::table()[index].flags & params::kReadonly) ||
            !activeParams[index].load(std::memory_order_relaxed))
            return;
        store.set(index, patchValue(index, physical));  // editor reads it back at once
        const double v = store.get(index);
        const auto i = static_cast<std::uint16_t>(index);
        if (gestureQueued && gestureIndex == static_cast<std::int32_t>(index)) {
            if (edits.free() > 2) edits.push({i, EditItem::Value, v});  // else: endEdit sends it
        } else if (gestureIndex != static_cast<std::int32_t>(index) && edits.free() >= 3) {
            edits.push({i, EditItem::Begin, 0.0});
            edits.push({i, EditItem::Value, v});
            edits.push({i, EditItem::End, 0.0});
        }
        requestFlush();
    }

    void endEdit(std::size_t index) override {
        const auto i = static_cast<std::uint16_t>(index);
        if (gestureQueued && gestureIndex == static_cast<std::int32_t>(index)) {
            edits.push({i, EditItem::Value, store.get(index)});
            edits.push({i, EditItem::End, 0.0});
        } else if (edits.free() >= 3) {  // the begin found no room: send it whole
            edits.push({i, EditItem::Begin, 0.0});
            edits.push({i, EditItem::Value, store.get(index)});
            edits.push({i, EditItem::End, 0.0});
        }
        gestureIndex = -1;
        gestureQueued = false;
        requestFlush();
    }

    Status merge(const json::Value& patch) override {
        return model.apply(CLAP_SUPPLE_SET_MERGE, patch);
    }
    std::string patchText(bool changedOnly) const override { return model.patchJson(changedOnly); }
    std::string describe() const override { return model.describe(true); }

    void setTelemetryEnabled(bool on) override { telemetryOn.store(on, std::memory_order_relaxed); }
    Telemetry telemetry() override {
        Telemetry t;
        t.peakL = peakL.exchange(0.0f);
        t.peakR = peakR.exchange(0.0f);
        t.voices = voices.load();
        t.processing = processing.load();
        return t;
    }

    void valuesChanged(bool structural) {
        if (hostParams) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        if (hostSemantic) {
            uint32_t flags = CLAP_SUPPLE_CHANGED_PATCH;
            if (structural) flags |= CLAP_SUPPLE_CHANGED_SCHEMA;
            hostSemantic->changed(host, flags);
        }
    }
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }
clapx::SemanticAdapter* adapterOf(const clap_plugin_t* p) { return &self(p)->adapter; }

Patch Model::current() const {
    Patch p;
    p.meta = meta;
    p.body.structure = structure;
    p.body.values = p_.store.snapshot().values;
    return p;
}

void Model::install(const Patch& p) {
    ++p_.structureRevision;
    meta = p.meta;
    structure = p.body.structure;
    for (int o = 0; o < kOscCount; ++o)
        p_.oscTypes[o].store(structure.osc[o] ? static_cast<int>(*structure.osc[o]) : -1,
                             std::memory_order_relaxed);
    for (std::size_t i = 0; i < paramCount(); ++i)
        p_.activeParams[i].store(orrery::paramActive(structure, i), std::memory_order_relaxed);
    p_.store.assign(patch::FlatBody{p.body.values});
    if (p_.active) p_.engine.setPatch(p);
}

Status Model::apply(std::uint32_t mode, const json::Value& document) {
    auto result = mode == CLAP_SUPPLE_SET_REPLACE ? type_.fromJson(document)
                                                  : type_.merge(current(), document);
    if (!result) return std::move(result).error();
    if (mode == CLAP_SUPPLE_SET_VALIDATE_ONLY) return okStatus();
    const bool structural = !(result->body.structure == structure);
    install(*result);
    p_.valuesChanged(structural);  // "active" flags in the schema follow the structure
    return okStatus();
}

// --- extensions -------------------------------------------------------------------------------

const clap_plugin_params_t kParams = {
    [](const clap_plugin_t*) { return clapx::ClapParams::count(orrery::table()); },
    [](const clap_plugin_t*, uint32_t i, clap_param_info_t* o) {
        return clapx::ClapParams::info(orrery::table(), i, o);
    },
    [](const clap_plugin_t* p, clap_id id, double* v) { return self(p)->store.getClap(id, *v); },
    [](const clap_plugin_t*, clap_id id, double v, char* o, uint32_t n) {
        return clapx::ClapParams::valueToText(orrery::table(), id, v, o, n);
    },
    [](const clap_plugin_t*, clap_id id, const char* t, double* v) {
        return clapx::ClapParams::textToValue(orrery::table(), id, t, v);
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
        Plugin* s = self(p);
        if (s->processing.load()) {
            // [audio thread] apply without rendering.
            s->events.clear();
            s->drainEdits(out, true);
            s->translate(in);
            s->engine.process(s->events, Transport{}, nullptr, nullptr, 0);
        } else {
            // [main thread] the store already holds editor values; they and
            // the host's reach the engine with the next program.
            const bool edited = s->drainEdits(out, false);
            if ((s->applyHostEvents(in) || edited) && s->active)
                s->engine.setPatch(s->model.current());
        }
    }};

const clap_plugin_state_t kState = {
    [](const clap_plugin_t* p, const clap_ostream_t* o) { return self(p)->adapter.saveState(o); },
    [](const clap_plugin_t* p, const clap_istream_t* i) {
        const bool ok = self(p)->adapter.loadState(i);
        return ok;
    }};

const clap_plugin_audio_ports_t kAudioPorts = {
    [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 0 : 1; },
    [](const clap_plugin_t* p, uint32_t index, bool input, clap_audio_port_info_t* info) {
        if (input || index != 0) return false;
        const bool mono = self(p)->mono;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        std::strcpy(info->name, "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
        info->channel_count = mono ? 1 : 2;
        info->port_type = mono ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }};

// Stereo (default) or mono output; mono is the stereo render's (L+R)/2.
const clap_plugin_audio_ports_config_t kAudioPortsConfig = {
    [](const clap_plugin_t*) -> uint32_t { return 2; },
    [](const clap_plugin_t*, uint32_t index, clap_audio_ports_config_t* c) {
        if (index > 1) return false;
        std::memset(c, 0, sizeof(*c));
        c->id = index;
        std::strcpy(c->name, index == 0 ? "Stereo" : "Mono");
        c->output_port_count = 1;
        c->has_main_output = true;
        c->main_output_channel_count = index == 0 ? 2 : 1;
        c->main_output_port_type = index == 0 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
        return true;
    },
    [](const clap_plugin_t* p, clap_id id) {
        Plugin* s = self(p);
        if (s->active || id > 1) return false;
        s->mono = id == 1;
        return true;
    }};

const clap_plugin_note_ports_t kNotePorts = {
    [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 1 : 0; },
    [](const clap_plugin_t*, uint32_t index, bool input, clap_note_port_info_t* info) {
        if (!input || index != 0) return false;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        info->supported_dialects =
            CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::strcpy(info->name, "Notes");
        return true;
    }};

const clap_plugin_latency_t kLatency = {[](const clap_plugin_t*) -> uint32_t { return 0; }};

// --- lifecycle --------------------------------------------------------------------------------

bool init(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->hostParams =
        static_cast<const clap_host_params_t*>(s->host->get_extension(s->host, CLAP_EXT_PARAMS));
    s->hostSemantic = static_cast<const clap_host_supple_semantic_params_t*>(
        s->host->get_extension(s->host, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    if (s->ext && s->ext->create) s->extData = s->ext->create(p, s->host, *s);
    return true;
}

bool activate(const clap_plugin_t* p, double sampleRate, uint32_t, uint32_t maxFrames) {
    Plugin* s = self(p);
    s->engine.prepare(sampleRate, maxFrames);
    s->events.reserve(kMaxEvents);
    s->scratchL.assign(maxFrames, 0.0f);
    s->scratchR.assign(maxFrames, 0.0f);
    s->active = true;
    s->engine.setPatch(s->model.current());
    return true;
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) {
    Plugin* s = self(p);
    s->events.clear();
    s->drainEdits(pr->out_events, true);  // editor values first (time 0), then the host's
    s->translate(pr->in_events);
    Transport t;
    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO))
        t.tempo = pr->transport->tempo;
    if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 1)
        return CLAP_PROCESS_ERROR;
    const clap_audio_buffer_t& ob = pr->audio_outputs[0];
    if (!ob.data32 && !ob.data64) return CLAP_PROCESS_ERROR;
    const std::uint32_t n = pr->frames_count;  // <= the activation's max_frames_count
    const bool wide = !ob.data32;
    const bool mono = ob.channel_count < 2;
    // Render straight into a stereo float output; otherwise into scratch, then
    // widen to 64-bit and/or fold to mono.
    const bool direct = !wide && !mono;
    float* left = direct ? ob.data32[0] : s->scratchL.data();
    float* right = direct ? ob.data32[1] : s->scratchR.data();
    s->engine.process(s->events, t, left, right, n);
    if (!direct) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = left[i], r = right[i];
            if (mono) {
                const float m = 0.5f * (l + r);
                if (wide)
                    ob.data64[0][i] = static_cast<double>(m);
                else
                    ob.data32[0][i] = m;
            } else {
                ob.data64[0][i] = static_cast<double>(l);
                ob.data64[1][i] = static_cast<double>(r);
            }
        }
    }
    if (!s->telemetryOn.load(std::memory_order_relaxed))
        return s->events.empty() && s->engine.silent() ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
    float pl = 0.0f, pr2 = 0.0f;
    for (std::uint32_t i = 0; i < pr->frames_count; ++i) {
        pl = std::max(pl, std::fabs(left[i]));
        pr2 = std::max(pr2, std::fabs(right[i]));
    }
    if (pl > s->peakL.load(std::memory_order_relaxed))
        s->peakL.store(pl, std::memory_order_relaxed);
    if (pr2 > s->peakR.load(std::memory_order_relaxed))
        s->peakR.store(pr2, std::memory_order_relaxed);
    s->voices.store(s->engine.activeVoices(), std::memory_order_relaxed);
    return s->events.empty() && s->engine.silent() ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
}

}  // namespace

const clap_plugin_descriptor_t* clapDescriptor() { return &kDescriptor; }

void* extensionData(const clap_plugin_t* plugin) { return self(plugin)->extData; }

const clap_plugin_t* createClapPlugin(const clap_host_t* host, const ClapExtensions* extensions) {
    if (!host || !clap_version_is_compatible(host->clap_version)) return nullptr;
    auto* s = new Plugin();
    s->host = host;
    s->ext = extensions;
    s->plugin = {&kDescriptor,
                 s,
                 init,
                 [](const clap_plugin_t* p) {
                     Plugin* me = self(p);
                     if (me->ext && me->ext->destroy && me->extData) me->ext->destroy(me->extData);
                     delete me;
                 },
                 activate,
                 [](const clap_plugin_t* p) { self(p)->active = false; },
                 [](const clap_plugin_t* p) {
                     self(p)->processing = true;
                     return true;
                 },
                 [](const clap_plugin_t* p) { self(p)->processing = false; },
                 [](const clap_plugin_t* p) { self(p)->engine.reset(); },
                 process,
                 [](const clap_plugin_t* p, const char* id) -> const void* {
                     if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
                     if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
                     if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
                     if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG)) return &kAudioPortsConfig;
                     if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
                     if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
                     if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS))
                         return clapx::SemanticAdapter::extension<adapterOf>();
                     const ClapExtensions* e = self(p)->ext;
                     return e && e->get ? e->get(id) : nullptr;
                 },
                 [](const clap_plugin_t* p) {
                     Plugin* me = self(p);
                     me->engine.collect();
                     if (me->retryFlush.exchange(false)) me->requestFlush();
                     if (me->ext && me->ext->onMainThread) me->ext->onMainThread(me->extData);
                 }};
    return &s->plugin;
}

}  // namespace supple::orrery
