#include "orrery/clap_plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#include "orrery/engine.h"
#include "orrery/patch.h"
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

struct Plugin {
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
    std::vector<Event> events;  // audio thread; reserved at activation
    std::array<std::int16_t, 2600> indexOfId{};
    bool active = false;
    std::atomic<bool> processing{false};

    Plugin() {
        indexOfId.fill(-1);
        const auto t = orrery::table();
        for (std::size_t i = 0; i < t.size(); ++i)
            if (t[i].id < indexOfId.size()) indexOfId[t[i].id] = static_cast<std::int16_t>(i);
        const Patch init = model.type().defaults();
        model.structure = init.body.structure;
        model.meta = init.meta;
        store.assign(patch::FlatBody{init.body.values});
    }

    // CLAP events → engine events (audio thread, no allocation: capacity reserved).
    void translate(const clap_input_events_t* in) SUPPLE_RT {
        events.clear();
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
                    store.setClap(pe->param_id, pe->value);
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
    meta = p.meta;
    structure = p.body.structure;
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
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
        Plugin* s = self(p);
        if (s->processing.load()) {
            // [audio thread] apply without rendering.
            s->translate(in);
            s->engine.process(s->events, Transport{}, nullptr, nullptr, 0);
        } else if (clapx::ClapParams::applyEvents(s->store, in) && s->active) {
            // [main thread] values reach the engine with the next program.
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
    [](const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info) {
        if (input || index != 0) return false;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        std::strcpy(info->name, "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
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
    return true;
}

bool activate(const clap_plugin_t* p, double sampleRate, uint32_t, uint32_t maxFrames) {
    Plugin* s = self(p);
    s->engine.prepare(sampleRate, maxFrames);
    s->events.reserve(kMaxEvents);
    s->active = true;
    s->engine.setPatch(s->model.current());
    return true;
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) {
    Plugin* s = self(p);
    s->translate(pr->in_events);
    Transport t;
    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO))
        t.tempo = pr->transport->tempo;
    if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 1)
        return CLAP_PROCESS_ERROR;
    float* left = pr->audio_outputs[0].data32[0];
    float* right = pr->audio_outputs[0].channel_count > 1 ? pr->audio_outputs[0].data32[1] : left;
    // CLAP guarantees frames_count <= the activation's max_frames_count.
    s->engine.process(s->events, t, left, right, pr->frames_count);
    return s->events.empty() && s->engine.silent() ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
}

}  // namespace

const clap_plugin_descriptor_t* clapDescriptor() { return &kDescriptor; }

const clap_plugin_t* createClapPlugin(const clap_host_t* host) {
    if (!host || !clap_version_is_compatible(host->clap_version)) return nullptr;
    auto* s = new Plugin();
    s->host = host;
    s->plugin = {&kDescriptor,
                 s,
                 init,
                 [](const clap_plugin_t* p) { delete self(p); },
                 activate,
                 [](const clap_plugin_t* p) { self(p)->active = false; },
                 [](const clap_plugin_t* p) {
                     self(p)->processing = true;
                     return true;
                 },
                 [](const clap_plugin_t* p) { self(p)->processing = false; },
                 [](const clap_plugin_t* p) { self(p)->engine.reset(); },
                 process,
                 [](const clap_plugin_t*, const char* id) -> const void* {
                     if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
                     if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
                     if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
                     if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
                     if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
                     if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS))
                         return clapx::SemanticAdapter::extension<adapterOf>();
                     return nullptr;
                 },
                 [](const clap_plugin_t* p) { self(p)->engine.collect(); }};
    return &s->plugin;
}

}  // namespace supple::orrery
