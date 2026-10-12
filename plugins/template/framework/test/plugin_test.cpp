// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The CLAP surface, in process, against a scripted host: identity, the
// parameter list, state, ports, bypass, the editor-to-host protocol under
// real threads, and the restart protocol. Run the tsan preset to have every
// cross-thread test here checked for races as well.
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <random>
#include <thread>
#include <vector>

#include <clap/ext/draft/tuning.h>

#include "Check.h"
#include "core/Engine.h"
#include "core/Files.h"
#include "core/ParamTable.h"
#include "core/Preset.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"
#include "dsp/AudioFile.h"
#include "gui/EditorHost.h"
#include "plugin/Entry.h"
#include "plugin/TestHooks.h"
#include "ProductInfo.h"

using namespace supple;

namespace {

// --- a scripted host ----------------------------------------------------------------

// Set around the calls a test makes as the host's audio thread, for a host
// with thread-check.
thread_local bool tAudioThread = false;
struct AudioScope {
    AudioScope() { tAudioThread = true; }
    ~AudioScope() { tAudioThread = false; }
};

#ifdef NDEBUG
constexpr bool kThreadChecks = false;
#else
constexpr bool kThreadChecks = true;
#endif

constexpr uint16_t kTuningSpace = 0x2345;  // where this host puts tuning events

struct Host {
    clap_host_t clap{};
    std::atomic<int> processRequests{0}, callbacks{0}, restarts{0}, flushRequests{0}, latencyChanges{0}, rescans{0}, tailChanges{0};
    std::atomic<int> dirty{0}, presetErrors{0};
    // The last preset the plugin said it loaded (preset-load.loaded).
    uint32_t loadedKind = 99;
    std::string loadedLocation, loadedKey;
    std::atomic<bool> callbackPending{false};
    std::atomic<std::thread::id> mainThread{std::this_thread::get_id()};
    std::atomic<bool> requestFlushOffMain{false};
    // What the plugin logged (main thread only, as it must).
    std::vector<std::pair<clap_log_severity, std::string>> logged;
    // Extensions a test switches on before init.
    bool threadCheck = false, contextMenu = false, tuning = false;
    // The track (track-info).
    std::string trackName = "Lead";
    bool trackColour = true;
    clap_color_t colour{255, 30, 160, 90};
    // Context menu: what perform() was asked.
    clap_id performedParam = CLAP_INVALID_ID, performedAction = CLAP_INVALID_ID;
    int noteNamesChanged = 0;

    size_t loggedContaining(const char* text) const {
        size_t n = 0;
        for (const auto& line : logged) n += line.second.find(text) != std::string::npos;
        return n;
    }

    Host() {
        clap.clap_version = CLAP_VERSION;
        clap.host_data = this;
        clap.name = "plugin-tests";
        clap.vendor = "";
        clap.url = "";
        clap.version = "1";
        clap.get_extension = extension;
        clap.request_restart = [](const clap_host_t* h) { self(h).restarts++; };
        clap.request_process = [](const clap_host_t* h) { self(h).processRequests++; };
        clap.request_callback = [](const clap_host_t* h) {
            self(h).callbacks++;
            self(h).callbackPending = true;
        };
    }
    static Host& self(const clap_host_t* h) { return *static_cast<Host*>(h->host_data); }
    static const void* CLAP_ABI extension(const clap_host_t* h, const char* id) {
        static const clap_host_params_t params{
            [](const clap_host_t* h, clap_param_rescan_flags) { self(h).rescans++; },
            [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
            [](const clap_host_t* h) {
                self(h).flushRequests++;
                if (std::this_thread::get_id() != self(h).mainThread.load()) self(h).requestFlushOffMain = true;
            }};
        static const clap_host_latency_t latency{[](const clap_host_t* h) { self(h).latencyChanges++; }};
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        static const clap_host_tail_t tail{[](const clap_host_t* h) { self(h).tailChanges++; }};
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
        if (!std::strcmp(id, CLAP_EXT_TAIL)) return &tail;
        static const clap_host_state_t state{[](const clap_host_t* h) { self(h).dirty++; }};
        if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
        static const clap_host_preset_load_t presetLoad{
            [](const clap_host_t* h, uint32_t, const char*, const char*, int32_t, const char*) {
                self(h).presetErrors++;
            },
            [](const clap_host_t* h, uint32_t kind, const char* location, const char* key) {
                self(h).loadedKind = kind;
                self(h).loadedLocation = location ? location : "";
                self(h).loadedKey = key ? key : "";
            }};
        if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &presetLoad;
        static const clap_host_log_t log{[](const clap_host_t* h, clap_log_severity severity, const char* text) {
            self(h).logged.push_back({severity, text ? text : ""});
        }};
        if (!std::strcmp(id, CLAP_EXT_LOG)) return &log;
        static const clap_host_thread_check_t threads{
            [](const clap_host_t* h) { return std::this_thread::get_id() == self(h).mainThread.load() && !tAudioThread; },
            [](const clap_host_t* h) { return tAudioThread || std::this_thread::get_id() != self(h).mainThread.load(); }};
        if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return self(h).threadCheck ? &threads : nullptr;
        static const clap_host_track_info_t track{[](const clap_host_t* h, clap_track_info_t* info) {
            const Host& host = self(h);
            *info = clap_track_info_t{};
            info->flags = CLAP_TRACK_INFO_HAS_TRACK_NAME | CLAP_TRACK_INFO_IS_FOR_BUS;
            std::snprintf(info->name, sizeof(info->name), "%s", host.trackName.c_str());
            if (host.trackColour) {
                info->flags |= CLAP_TRACK_INFO_HAS_TRACK_COLOR;
                info->color = host.colour;
            }
            return true;
        }};
        if (!std::strcmp(id, CLAP_EXT_TRACK_INFO)) return &track;
        static const clap_host_context_menu_t menu{
            [](const clap_host_t*, const clap_context_menu_target_t* target, const clap_context_menu_builder_t* b) {
                if (!target || target->kind != CLAP_CONTEXT_MENU_TARGET_KIND_PARAM) return false;
                for (uint32_t kind = CLAP_CONTEXT_MENU_ITEM_ENTRY; kind <= CLAP_CONTEXT_MENU_ITEM_TITLE; ++kind)
                    if (!b->supports(b, kind)) return false;
                const clap_context_menu_entry_t automate{"Automate", true, 10};
                const clap_context_menu_submenu_t midi{"MIDI", true};
                const clap_context_menu_check_entry_t learn{"Learn", true, true, 11};
                const clap_context_menu_item_title_t title{"Title", true};
                return b->add_item(b, CLAP_CONTEXT_MENU_ITEM_ENTRY, &automate) &&
                       b->add_item(b, CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU, &midi) &&
                       b->add_item(b, CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY, &learn) &&
                       b->add_item(b, CLAP_CONTEXT_MENU_ITEM_END_SUBMENU, nullptr) &&
                       b->add_item(b, CLAP_CONTEXT_MENU_ITEM_SEPARATOR, nullptr) &&
                       b->add_item(b, CLAP_CONTEXT_MENU_ITEM_TITLE, &title);
            },
            [](const clap_host_t* h, const clap_context_menu_target_t* target, clap_id action) {
                self(h).performedParam = target ? target->id : CLAP_INVALID_ID;
                self(h).performedAction = action;
                return true;
            },
            [](const clap_host_t*) { return false; },
            [](const clap_host_t*, const clap_context_menu_target_t*, int32_t, int32_t, int32_t) { return false; }};
        if (!std::strcmp(id, CLAP_EXT_CONTEXT_MENU)) return self(h).contextMenu ? &menu : nullptr;
        static const clap_host_note_name_t names{[](const clap_host_t* h) { self(h).noteNamesChanged++; }};
        if (!std::strcmp(id, CLAP_EXT_NOTE_NAME)) return &names;
        // Tuning 7 is a quarter tone sharp with key 61 unmapped; any other
        // is equal temperament.
        static const clap_host_event_registry_t registry{
            [](const clap_host_t*, const char* space, uint16_t* out) {
                if (std::strcmp(space, CLAP_EXT_TUNING) != 0) return false;
                *out = kTuningSpace;
                return true;
            }};
        static const clap_host_tuning_t tunings{
            [](const clap_host_t*, clap_id tuning, int32_t, int32_t, uint32_t) { return tuning == 7 ? 0.5 : 0.0; },
            [](const clap_host_t*, clap_id tuning, int32_t, int32_t key) { return !(tuning == 7 && key == 61); },
            [](const clap_host_t*) -> uint32_t { return 1; },
            [](const clap_host_t*, uint32_t index, clap_tuning_info_t* info) {
                if (index != 0) return false;
                info->tuning_id = 7;
                std::snprintf(info->name, sizeof(info->name), "Quarter tone up");
                info->is_dynamic = false;
                return true;
            }};
        if (!std::strcmp(id, CLAP_EXT_EVENT_REGISTRY)) return self(h).tuning ? &registry : nullptr;
        if (!std::strcmp(id, CLAP_EXT_TUNING)) return self(h).tuning ? &tunings : nullptr;
        return nullptr;
    }
};

const clap_plugin_factory_t* factory() {
    return static_cast<const clap_plugin_factory_t*>(supple_entry_get_factory(CLAP_PLUGIN_FACTORY_ID));
}

struct Instance {
    Host host;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    bool active = false;

    // `setup` prepares the host before the plugin is created.
    explicit Instance(const std::function<void(Host&)>& setup = {}) {
        if (setup) setup(host);
        plugin = factory()->create_plugin(factory(), &host.clap, product::info::kId);
        plugin->init(plugin);
        params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    }
    ~Instance() {
        if (active) plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    void activate(double sr = 48000.0, uint32_t maxFrames = 512) {
        active = plugin->activate(plugin, sr, 1, maxFrames);
        plugin->start_processing(plugin);
    }
    // The default configuration is the first declared layout.
    int channels() const { return product::info::kChannelLayouts[0]; }
    void pumpMain() {
        if (host.callbackPending.exchange(false)) plugin->on_main_thread(plugin);
    }
    double value(size_t i) const {
        double v = 0.0;
        params->get_value(plugin, params::at(i).id, &v);
        return v;
    }
};

struct InEvents {
    std::vector<clap_event_param_value_t> values;
    clap_input_events_t list{this, [](const clap_input_events_t* l) {
                                 return static_cast<uint32_t>(static_cast<const InEvents*>(l->ctx)->values.size());
                             },
                             [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
                                 return &static_cast<const InEvents*>(l->ctx)->values[i].header;
                             }};
    void add(uint32_t time, uint32_t id, double v) {
        clap_event_param_value_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = id;
        e.note_id = -1;
        e.port_index = e.channel = e.key = -1;
        e.value = v;
        values.push_back(e);
    }
};

// Records what the plugin tells the host, refusing a share of it on request.
// A host's output event list. It is called on the audio thread, so it
// allocates nothing: the rtsan preset would rightly fail a host that did.
struct OutEvents {
    int depth[params::kCount] = {};
    double lastValue[params::kCount] = {};
    bool sawValue[params::kCount] = {};
    std::vector<clap_event_note_t> noteEnds, notes;
    std::vector<clap_event_midi_t> midi;
    bool nested = false, strayEnd = false, unknownParam = false, outOfOrder = false;
    uint32_t lastTime = 0;  // within the current block; newBlock() resets it
    double refuse = 0.0;
    std::mt19937 rng{3};
    clap_output_events_t list{this, [](const clap_output_events_t* l, const clap_event_header_t* h) {
                                  return static_cast<OutEvents*>(l->ctx)->push(h);
                              }};
    OutEvents() {
        noteEnds.reserve(4096);
        notes.reserve(4096);
        midi.reserve(4096);
    }
    void newBlock() { lastTime = 0; }
    // For a flush: a list of its own, in time order from 0.
    const clap_output_events_t* fresh() {
        newBlock();
        return &list;
    }
    bool push(const clap_event_header_t* h) {
        if (refuse > 0.0 && std::uniform_real_distribution<double>(0, 1)(rng) < refuse) return false;
        // A host's list must arrive in time order.
        outOfOrder |= h->time < lastTime;
        lastTime = h->time;
        if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
            const auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(h);
            const int i = params::indexOf(g->param_id);
            if (i < 0) return unknownParam = true;
            if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN)
                nested |= ++depth[i] > 1;
            else
                strayEnd |= --depth[i] < 0;
        } else if (h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
            const int i = params::indexOf(v->param_id);
            if (i < 0) return unknownParam = true;
            lastValue[i] = v->value;
            sawValue[i] = true;
        } else if (h->type == CLAP_EVENT_NOTE_END) {
            if (noteEnds.size() < noteEnds.capacity())
                noteEnds.push_back(*reinterpret_cast<const clap_event_note_t*>(h));
        } else if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE) {
            if (notes.size() < notes.capacity()) notes.push_back(*reinterpret_cast<const clap_event_note_t*>(h));
        } else if (h->type == CLAP_EVENT_MIDI) {
            if (midi.size() < midi.capacity()) midi.push_back(*reinterpret_cast<const clap_event_midi_t*>(h));
        }
        return true;
    }
    bool balanced() const {
        for (int d : depth)
            if (d != 0) return false;
        return !nested && !strayEnd && !unknownParam && !outOfOrder;
    }
};

// Every bus the product declares: the main bus in place (as most hosts run
// an effect), the sidechain and each aux output in storage of their own.
struct Audio {
    int channels;
    std::vector<std::vector<float>> data;                   // the main bus
    std::vector<std::vector<float>> sidechain;              // PRODUCT_SIDECHAIN
    std::vector<std::vector<std::vector<float>>> aux;       // PRODUCT_AUX_OUTPUTS
    std::vector<float*> pointers, sidechainPointers;
    std::vector<std::vector<float*>> auxPointers;
    std::vector<clap_audio_buffer_t> ins, outs;
    clap_process_t process{};
    explicit Audio(int ch, uint32_t frames = 256)
        : channels(ch), data(static_cast<size_t>(ch), std::vector<float>(frames)) {
        const auto width = static_cast<uint32_t>(ch);
        for (auto& d : data) pointers.push_back(d.data());
        auto bus = [&](float** p) {
            clap_audio_buffer_t b{};
            b.data32 = p;
            b.channel_count = width;
            return b;
        };
        if (product::info::kSidechain) {
            sidechain.assign(static_cast<size_t>(ch), std::vector<float>(frames));
            for (auto& d : sidechain) sidechainPointers.push_back(d.data());
        }
        aux.assign(static_cast<size_t>(product::info::kAuxOutputs),
                   std::vector<std::vector<float>>(static_cast<size_t>(ch), std::vector<float>(frames)));
        for (auto& b : aux) {
            auxPointers.emplace_back();
            for (auto& d : b) auxPointers.back().push_back(d.data());
        }
        if (product::info::kAudioInput) ins.push_back(bus(pointers.data()));
        if (product::info::kSidechain) ins.push_back(bus(sidechainPointers.data()));
        if (product::info::kAudioOutput) outs.push_back(bus(pointers.data()));
        for (auto& p : auxPointers) outs.push_back(bus(p.data()));
        process.frames_count = frames;
        connect(true, true);
    }
    // A host that leaves the sidechain or the aux outputs unconnected
    // passes fewer buffers.
    void connect(bool sidechainToo, bool auxToo) {
        const uint32_t mainIn = product::info::kAudioInput ? 1 : 0, mainOut = product::info::kAudioOutput ? 1 : 0;
        process.audio_inputs = ins.empty() ? nullptr : ins.data();
        process.audio_inputs_count = sidechainToo ? static_cast<uint32_t>(ins.size()) : mainIn;
        process.audio_outputs = outs.empty() ? nullptr : outs.data();
        process.audio_outputs_count = auxToo ? static_cast<uint32_t>(outs.size()) : mainOut;
    }
    clap_audio_buffer_t* mainOut() { return product::info::kAudioOutput ? &outs[0] : nullptr; }
    clap_process_status run(const clap_plugin_t* p, const clap_input_events_t* events, OutEvents* output) {
        process.in_events = events;
        process.out_events = output ? &output->list : nullptr;
        if (output) output->newBlock();
        return p->process(p, &process);
    }
};

const clap_input_events_t* noEvents() {
    static const clap_input_events_t none{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
                                          [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
                                              return nullptr;
                                          }};
    return &none;
}

std::vector<uint8_t> saveState(Instance& i) {
    std::vector<uint8_t> bytes;
    clap_ostream_t os{&bytes, [](const clap_ostream_t* s, const void* p, uint64_t n) -> int64_t {
                          auto* b = static_cast<std::vector<uint8_t>*>(s->ctx);
                          b->insert(b->end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
                          return static_cast<int64_t>(n);
                      }};
    i.state->save(i.plugin, &os);
    return bytes;
}

struct ByteReader {
    const std::vector<uint8_t>* bytes;
    size_t at = 0;
    clap_istream_t stream{this, [](const clap_istream_t* s, void* p, uint64_t n) -> int64_t {
                              auto* r = static_cast<ByteReader*>(s->ctx);
                              const size_t take = std::min<size_t>(static_cast<size_t>(n), r->bytes->size() - r->at);
                              if (take) std::memcpy(p, r->bytes->data() + r->at, take);
                              r->at += take;
                              return static_cast<int64_t>(take);
                          }};
};

const clap_plugin_state_context_t* stateContext(Instance& i) {
    return static_cast<const clap_plugin_state_context_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_STATE_CONTEXT));
}

std::vector<uint8_t> saveState(Instance& i, uint32_t context) {
    std::vector<uint8_t> bytes;
    clap_ostream_t os{&bytes, [](const clap_ostream_t* s, const void* p, uint64_t n) -> int64_t {
                          auto* b = static_cast<std::vector<uint8_t>*>(s->ctx);
                          b->insert(b->end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
                          return static_cast<int64_t>(n);
                      }};
    stateContext(i)->save(i.plugin, &os, context);
    return bytes;
}

bool loadState(Instance& i, const std::vector<uint8_t>& bytes, uint32_t context) {
    ByteReader reader{&bytes};
    return stateContext(i)->load(i.plugin, &reader.stream, context);
}

// Points the user preset folder somewhere of the test's own while it lives.
struct PresetFolder {
    std::filesystem::path path;
    PresetFolder() {
        path = std::filesystem::temp_directory_path() / ("supple-plugin-presets-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
        set(PresetLibrary::toUtf8(path).c_str());
    }
    ~PresetFolder() {
        set("");
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    static void set(const char* value) {
#if defined(_WIN32)
        _putenv_s("SUPPLE_PRESETS_DIR", value);
#else
        if (*value) setenv("SUPPLE_PRESETS_DIR", value, 1);
        else unsetenv("SUPPLE_PRESETS_DIR");
#endif
    }
};

bool loadState(Instance& i, const std::vector<uint8_t>& bytes) {
    struct Reader {
        const std::vector<uint8_t>* bytes;
        size_t at = 0;
    } reader{&bytes};
    clap_istream_t is{&reader, [](const clap_istream_t* s, void* p, uint64_t n) -> int64_t {
                          auto* r = static_cast<Reader*>(s->ctx);
                          const size_t take = std::min<size_t>(static_cast<size_t>(n), r->bytes->size() - r->at);
                          if (take) std::memcpy(p, r->bytes->data() + r->at, take);
                          r->at += take;
                          return static_cast<int64_t>(take);
                      }};
    return i.state->load(i.plugin, &is);
}

}  // namespace

namespace {

struct NoteList {
    std::vector<clap_event_note_t> notes;
    clap_input_events_t list{this, [](const clap_input_events_t* l) {
                                 return static_cast<uint32_t>(static_cast<const NoteList*>(l->ctx)->notes.size());
                             },
                             [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
                                 return &static_cast<const NoteList*>(l->ctx)->notes[i].header;
                             }};
    void add(uint16_t type, uint32_t time, int key, int channel, int id) {
        clap_event_note_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = type;
        e.note_id = id;
        e.port_index = 0;
        e.channel = static_cast<int16_t>(channel);
        e.key = static_cast<int16_t>(key);
        e.velocity = 0.8;
        notes.push_back(e);
    }
};

}  // namespace

// --- identity and parameters ------------------------------------------------------------

TEST(the_descriptor_is_the_product_identity) {
    CHECK(factory()->get_plugin_count(factory()) == 1);
    const clap_plugin_descriptor_t* d = factory()->get_plugin_descriptor(factory(), 0);
    CHECK(!std::strcmp(d->id, product::info::kId));
    CHECK(!std::strcmp(d->name, product::info::kName));
    CHECK(!std::strcmp(d->version, product::info::kVersion));
    CHECK(!std::strcmp(d->vendor, product::info::kVendor));
    CHECK(d->features && d->features[0] &&
          !std::strcmp(d->features[0], product::info::kInstrument    ? CLAP_PLUGIN_FEATURE_INSTRUMENT
                                       : product::info::kNoteEffect ? CLAP_PLUGIN_FEATURE_NOTE_EFFECT
                                                                    : CLAP_PLUGIN_FEATURE_AUDIO_EFFECT));
    Host host;
    CHECK(factory()->create_plugin(factory(), &host.clap, "com.example.not-this") == nullptr);
}

TEST(the_host_parameter_list_is_the_table) {
    Instance i;
    CHECK(i.params->count(i.plugin) == params::kCount);
    for (uint32_t k = 0; k < i.params->count(i.plugin); ++k) {
        clap_param_info_t info{};
        CHECK(i.params->get_info(i.plugin, k, &info));
        const ParamDef& p = params::at(k);
        CHECK(info.id == p.id);
        CHECK(!std::strcmp(info.name, p.name));
        CHECK(info.min_value == p.hostMin() && info.max_value == p.hostMax());
        CHECK(info.default_value == hostDefault(p));
        CHECK(((info.flags & CLAP_PARAM_IS_BYPASS) != 0) == p.is(pflag::kBypass));
        CHECK(((info.flags & CLAP_PARAM_IS_STEPPED) != 0) == p.stepped());
        CHECK(((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0) == p.is(pflag::kAutomatable));
        CHECK(((info.flags & CLAP_PARAM_IS_READONLY) != 0) == p.isOutput());
        CHECK(i.value(k) == hostDefault(p));
    }
    clap_param_info_t none{};
    CHECK(!i.params->get_info(i.plugin, static_cast<uint32_t>(params::kCount), &none));
}

// --- state ------------------------------------------------------------------------------

TEST(state_restores_every_value_and_resaves_identically) {
    std::vector<uint8_t> saved;
    std::vector<double> expected(params::kCount);
    {
        Instance a;
        a.activate();
        Audio audio(a.channels());
        InEvents events;
        for (size_t k = 0; k < params::kCount; ++k) {
            // An output is not the host's to set, nor state to restore.
            expected[k] = params::at(k).isOutput() ? hostDefault(params::at(k)) : fromNormalized(params::at(k), 0.63);
            events.add(0, params::at(k).id, fromNormalized(params::at(k), 0.63));
        }
        OutEvents out;
        audio.run(a.plugin, &events.list, &out);
        saved = saveState(a);
    }
    Instance b;
    CHECK(loadState(b, saved));
    for (size_t k = 0; k < params::kCount; ++k) CHECK_MSG(b.value(k) == expected[k], "%s", params::at(k).name);
    CHECK(saveState(b) == saved);
    CHECK(b.host.rescans >= 1);
    // Damaged state is refused and changes nothing.
    std::vector<uint8_t> damaged(saved.begin(), saved.begin() + saved.size() / 2);
    Instance c;
    CHECK(!loadState(c, damaged));
    CHECK(!loadState(c, {}));
    for (size_t k = 0; k < params::kCount; ++k) CHECK(c.value(k) == hostDefault(params::at(k)));
}

// --- properties, state context and presets ------------------------------------------------

TEST(properties_are_state_and_reach_the_processor_only_through_their_store) {
    if constexpr (properties::kCount == 0) {
        std::puts("      (no properties in this product)");
        return;
    }
    std::vector<uint8_t> saved;
    {
        Instance a;
        gui::EditorHost* editor = supple_test_editor_host(a.plugin);
        if (!editor) {
            std::puts("      (no editor host in this build)");
            return;
        }
        for (size_t k = 0; k < properties::kCount; ++k) {
            CHECK(editor->property(properties::at(k).id) == properties::at(k).def);
            const uint64_t version = editor->propertiesVersion();
            CHECK(editor->setProperty(properties::at(k).id, "v" + std::to_string(k)));
            CHECK(editor->propertiesVersion() != version);
        }
        CHECK(a.host.dirty == static_cast<int>(properties::kCount));
        // Too long: refused, and nothing to save.
        CHECK(!editor->setProperty(properties::at(0).id, std::string(properties::at(0).limit + 1, 'x')));
        CHECK(!editor->setProperty(0xFFFFFFF0u, "nobody"));
        CHECK(a.host.dirty == static_cast<int>(properties::kCount));
        saved = saveState(a);
    }
    Instance b;
    CHECK(loadState(b, saved));
    gui::EditorHost* editor = supple_test_editor_host(b.plugin);
    for (size_t k = 0; editor && k < properties::kCount; ++k)
        CHECK(editor->property(properties::at(k).id) == "v" + std::to_string(k));
    CHECK(saveState(b) == saved);
    CHECK(b.host.dirty == 0);  // the host loaded it: nothing to tell it
}

TEST(a_session_finds_the_files_it_names_after_they_move) {
    int k = -1;
    for (size_t i = 0; i < properties::kCount && k < 0; ++i)
        if (properties::at(i).isFile() && files::hasExtension("x.wav", properties::at(i).extensions)) k = static_cast<int>(i);
    if (k < 0) {
        std::puts("      (no file property taking WAV in this product)");
        return;
    }
    const uint32_t id = properties::at(static_cast<size_t>(k)).id;
    // Its own settings folder, so remembered folders stay out of the user's.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("supple-plugin-files-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root / "settings");
    auto setSettings = [](const std::string& value) {
#if defined(_WIN32)
        _putenv_s("SUPPLE_SETTINGS_DIR", value.c_str());
#else
        if (!value.empty()) setenv("SUPPLE_SETTINGS_DIR", value.c_str(), 1);
        else unsetenv("SUPPLE_SETTINGS_DIR");
#endif
    };
    setSettings(files::toUtf8(root / "settings"));
    dsp::AudioData audio;
    audio.sampleRate = 48000.0;
    audio.channel.assign(2, std::vector<float>(4800, 0.0f));
    audio.channel[0][0] = audio.channel[1][0] = 1.0f;
    std::filesystem::create_directories(root / "old" / "Kit");
    const std::string before = files::toUtf8(root / "old" / "Kit" / "ir.wav");
    CHECK(dsp::saveWavFile(before, audio));

    std::vector<uint8_t> saved;
    {
        Instance a;
        gui::EditorHost* editor = supple_test_editor_host(a.plugin);
        if (!editor) {
            std::puts("      (no editor host in this build)");
            setSettings("");
            return;
        }
        CHECK(editor->setProperty(id, before));
        a.activate();
        CHECK(editor->fileStatus(id).state == FileStatus::State::Ready);
        saved = saveState(a);
    }
    // The library moves; the user has picked from it since.
    std::filesystem::rename(root / "old", root / "new");
    files::rememberFolder(files::toUtf8(root / "new"));
    const std::string after = files::toUtf8(root / "new" / "Kit" / "ir.wav");
    {
        Instance b;
        CHECK(loadState(b, saved));
        gui::EditorHost* editor = supple_test_editor_host(b.plugin);
        CHECK(editor && editor->property(id) == after);
        b.activate();
        CHECK(editor && editor->fileStatus(id).state == FileStatus::State::Ready);
    }
    // Gone altogether: the reference is kept, and saved as it was.
    std::error_code ec;
    std::filesystem::remove_all(root / "new", ec);
    {
        Instance c;
        CHECK(loadState(c, saved));
        gui::EditorHost* editor = supple_test_editor_host(c.plugin);
        CHECK(editor && editor->property(id) == before);
        CHECK(editor && editor->fileStatus(id).state == FileStatus::State::Missing);
        CHECK(saveState(c) == saved);
    }
    setSettings("");
    std::filesystem::remove_all(root, ec);
}

TEST(the_preset_context_leaves_the_session_alone) {
    Instance a;
    CHECK(stateContext(a) != nullptr);
    gui::EditorHost* editor = supple_test_editor_host(a.plugin);
    for (size_t k = 0; editor && k < properties::kCount; ++k) editor->setProperty(properties::at(k).id, "from a");
    const std::vector<uint8_t> preset = saveState(a, CLAP_STATE_CONTEXT_FOR_PRESET);
    const std::vector<uint8_t> project = saveState(a, CLAP_STATE_CONTEXT_FOR_PROJECT);
    CHECK(project == saveState(a) && saveState(a, CLAP_STATE_CONTEXT_FOR_DUPLICATE) == project);
    CHECK(preset.size() <= project.size());

    Instance b;
    gui::EditorHost* other = supple_test_editor_host(b.plugin);
    for (size_t k = 0; other && k < properties::kCount; ++k) other->setProperty(properties::at(k).id, "from b");
    CHECK(loadState(b, project, CLAP_STATE_CONTEXT_FOR_PRESET));  // a whole session, as a preset
    for (size_t k = 0; other && k < properties::kCount; ++k) {
        const bool session = properties::at(k).perProjectOnly();
        CHECK_MSG(other->property(properties::at(k).id) == (session ? "from b" : "from a"), "%s", properties::at(k).name);
    }
    CHECK(loadState(b, preset, CLAP_STATE_CONTEXT_FOR_PROJECT));  // a preset as a session: defaults the rest
    for (size_t k = 0; other && k < properties::kCount; ++k) {
        const bool session = properties::at(k).perProjectOnly();
        CHECK(other->property(properties::at(k).id) == (session ? properties::at(k).def : "from a"));
    }
}

TEST(hosts_load_presets_by_location_and_hear_of_the_editors) {
    if constexpr (!presets::kEnabled) {
        CHECK(supple_entry_get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID) == nullptr);
        std::puts("      (presets are off in this product)");
        return;
    }
    PresetFolder folder;
    Instance i;
    const auto* load = static_cast<const clap_plugin_preset_load_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_PRESET_LOAD));
    CHECK(load != nullptr);
    if (!load) return;
    for (size_t k = 0; k < presets::kFactoryCount; ++k)
        CHECK(load->from_location(i.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, presets::factory(k).name));
    CHECK(i.host.presetErrors == 0);
    CHECK(!load->from_location(i.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "No Such Preset"));
    CHECK(i.host.presetErrors == 1);

    // A file: a preset saved by another instance.
    Instance other;
    const std::filesystem::path file = folder.path / "Elsewhere.preset";
    {
        const std::vector<uint8_t> bytes = saveState(other, CLAP_STATE_CONTEXT_FOR_PRESET);
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                    static_cast<std::streamsize>(bytes.size()));
    }
    const std::string path = PresetLibrary::toUtf8(file);
    CHECK(load->from_location(i.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), nullptr));
    CHECK(!load->from_location(i.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, (path + ".missing").c_str(), nullptr));
    CHECK(i.host.presetErrors == 2);

    // The editor's own loads are reported to the host, and the session
    // remembers which preset it is.
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    PresetLibrary* library = editor ? editor->presetLibrary() : nullptr;
    if (!library) return;
    library->refresh();
    CHECK(library->entries().size() == presets::kFactoryCount + 1);
    if (presets::kFactoryCount > 0) {
        CHECK(library->load(0));
        CHECK(i.host.loadedKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && i.host.loadedKey == presets::factory(0).name);
        Instance restored;
        CHECK(loadState(restored, saveState(i)));
        PresetLibrary* again = supple_test_editor_host(restored.plugin)->presetLibrary();
        CHECK(again->currentName() == presets::factory(0).name && !again->modified());
    }
    const int user = static_cast<int>(presets::kFactoryCount);
    CHECK(library->load(user));
    CHECK(i.host.loadedKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && i.host.loadedLocation == path);
    CHECK(i.host.dirty >= 1);
}

namespace {

// A preset browser's side of discovery: what the provider declares and says.
struct Indexer {
    clap_preset_discovery_indexer_t clap{};
    std::vector<std::string> extensions;
    std::vector<std::pair<uint32_t, std::string>> locations;  // kind, location
    Indexer() {
        clap.clap_version = CLAP_VERSION;
        clap.name = "plugin-tests";
        clap.indexer_data = this;
        clap.declare_filetype = [](const clap_preset_discovery_indexer_t* x, const clap_preset_discovery_filetype_t* t) {
            static_cast<Indexer*>(x->indexer_data)->extensions.push_back(t->file_extension ? t->file_extension : "");
            return true;
        };
        clap.declare_location = [](const clap_preset_discovery_indexer_t* x, const clap_preset_discovery_location_t* l) {
            static_cast<Indexer*>(x->indexer_data)->locations.push_back({l->kind, l->location ? l->location : ""});
            return true;
        };
        clap.declare_soundpack = [](const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) {
            return true;
        };
        clap.get_extension = [](const clap_preset_discovery_indexer_t*, const char*) -> const void* { return nullptr; };
    }
};

struct Receiver {
    clap_preset_discovery_metadata_receiver_t clap{};
    std::vector<std::pair<std::string, std::string>> presets;  // name, load key
    std::vector<std::string> pluginIds;
    int errors = 0;
    Receiver() {
        clap.receiver_data = this;
        clap.on_error = [](const clap_preset_discovery_metadata_receiver_t* r, int32_t, const char*) { self(r).errors++; };
        clap.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key) {
            self(r).presets.push_back({name ? name : "", key ? key : ""});
            return true;
        };
        clap.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id) {
            self(r).pluginIds.push_back(std::string(id->abi) + ":" + id->id);
        };
        clap.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        clap.set_flags = [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {};
        clap.add_creator = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        clap.set_description = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        clap.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
        clap.add_feature = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        clap.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};
    }
    static Receiver& self(const clap_preset_discovery_metadata_receiver_t* r) {
        return *static_cast<Receiver*>(r->receiver_data);
    }
};

}  // namespace

TEST(preset_discovery_lists_the_factory_and_the_users_folder) {
    if constexpr (!presets::kEnabled) {
        std::puts("      (presets are off in this product)");
        return;
    }
    PresetFolder folder;
    const auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
        supple_entry_get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    CHECK(factory && factory->count(factory) == 1);
    if (!factory) return;
    const clap_preset_discovery_provider_descriptor_t* d = factory->get_descriptor(factory, 0);
    Indexer indexer;
    const clap_preset_discovery_provider_t* provider = factory->create(factory, &indexer.clap, d->id);
    CHECK(provider && provider->init(provider));
    CHECK(indexer.extensions.size() == 1 && indexer.extensions[0] == "preset");
    CHECK(indexer.locations.size() == (presets::kFactoryCount > 0 ? 2u : 1u));

    Receiver factoryPresets;
    CHECK(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &factoryPresets.clap));
    CHECK(factoryPresets.presets.size() == presets::kFactoryCount);
    for (size_t k = 0; k < factoryPresets.presets.size(); ++k)
        CHECK(factoryPresets.presets[k].first == presets::factory(k).name &&
              factoryPresets.presets[k].second == presets::factory(k).name);
    for (const std::string& id : factoryPresets.pluginIds) CHECK(id == std::string("clap:") + product::info::kId);

    Instance i;
    const std::filesystem::path file = folder.path / "Mine.preset";
    {
        const std::vector<uint8_t> bytes = saveState(i, CLAP_STATE_CONTEXT_FOR_PRESET);
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                    static_cast<std::streamsize>(bytes.size()));
        std::ofstream(folder.path / "Junk.preset") << "not a preset";
    }
    Receiver user;
    CHECK(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, PresetLibrary::toUtf8(file).c_str(),
                                 &user.clap));
    CHECK(user.presets.size() == 1 && user.presets[0].first == "Mine" && user.errors == 0);
    Receiver junk;
    CHECK(!provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                  PresetLibrary::toUtf8(folder.path / "Junk.preset").c_str(), &junk.clap));
    CHECK(junk.presets.empty() && junk.errors == 1);
    const auto huge = folder.path / "Huge.preset";
    std::ofstream(huge).close();
    std::filesystem::resize_file(huge, state::kMaxBytes + 1);
    const auto directory = folder.path / "Directory.preset";
    std::filesystem::create_directory(directory);
    for (const auto& bad : {huge, directory, folder.path / "Unreadable.preset"}) {
        Receiver rejected;
        CHECK(!provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                      PresetLibrary::toUtf8(bad).c_str(), &rejected.clap));
        CHECK(rejected.presets.empty() && rejected.errors == 1);
    }
    provider->destroy(provider);
    CHECK(factory->create(factory, &indexer.clap, "someone.else") == nullptr);
}

// --- ports ------------------------------------------------------------------------------

TEST(every_declared_layout_activates_and_processes) {
    for (int channels : product::info::kChannelLayouts) {
        Instance i;
        auto* configs = static_cast<const clap_plugin_audio_ports_config_t*>(
            i.plugin->get_extension(i.plugin, CLAP_EXT_AUDIO_PORTS_CONFIG));
        auto* ports = static_cast<const clap_plugin_audio_ports_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_AUDIO_PORTS));
        CHECK(ports != nullptr);
        if (product::info::kAudioOutput) {
            CHECK(configs && configs->select(i.plugin, static_cast<clap_id>(channels)));
            // Every port takes the layout's width.
            for (int dir = 0; dir < 2; ++dir)
                for (uint32_t k = 0; k < ports->count(i.plugin, dir == 0); ++k) {
                    clap_audio_port_info_t info{};
                    CHECK(ports->get(i.plugin, k, dir == 0, &info) && info.channel_count == static_cast<uint32_t>(channels));
                }
        } else {
            CHECK(configs == nullptr && ports->count(i.plugin, true) == 0 && ports->count(i.plugin, false) == 0);
        }
        i.activate();
        if (configs) CHECK(!configs->select(i.plugin, static_cast<clap_id>(channels)));  // not while active
        Audio audio(channels);
        OutEvents events;
        CHECK(audio.run(i.plugin, noEvents(), &events) == CLAP_PROCESS_CONTINUE);
        // A host that connects nothing to the sidechain or aux outputs.
        audio.connect(false, false);
        CHECK(audio.run(i.plugin, noEvents(), &events) == CLAP_PROCESS_CONTINUE);
        if (channels > 1) {
            Audio narrow(channels - 1);  // the host breaking its own contract
            CHECK(narrow.run(i.plugin, noEvents(), &events) == CLAP_PROCESS_ERROR);
        }
    }
}

TEST(a_host_negotiates_layouts_port_by_port) {
    if (!product::info::kAudioOutput) return;
    Instance i;
    auto* configurable = static_cast<const clap_plugin_configurable_audio_ports_t*>(
        i.plugin->get_extension(i.plugin, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS));
    auto* ports = static_cast<const clap_plugin_audio_ports_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_AUDIO_PORTS));
    CHECK(configurable && ports);
    if (!configurable || !ports) return;
    auto requests = [&](uint32_t width, uint32_t odd = 0) {
        std::vector<clap_audio_port_configuration_request_t> r;
        for (int dir = 0; dir < 2; ++dir)
            for (uint32_t k = 0; k < ports->count(i.plugin, dir == 0); ++k)
                r.push_back({dir == 0, k, r.empty() ? width : width + odd, nullptr, nullptr});
        return r;
    };
    for (int channels : product::info::kChannelLayouts) {
        const auto r = requests(static_cast<uint32_t>(channels));
        CHECK(configurable->can_apply_configuration(i.plugin, r.data(), static_cast<uint32_t>(r.size())));
        CHECK(configurable->apply_configuration(i.plugin, r.data(), static_cast<uint32_t>(r.size())));
        clap_audio_port_info_t out{};
        CHECK(ports->get(i.plugin, 0, false, &out) && out.channel_count == static_cast<uint32_t>(channels));
        if (r.size() > 1) {  // ports may not disagree
            const auto mixed = requests(static_cast<uint32_t>(channels), 1);
            CHECK(!configurable->apply_configuration(i.plugin, mixed.data(), static_cast<uint32_t>(mixed.size())));
        }
    }
    const auto undeclared = requests(65);
    CHECK(!configurable->apply_configuration(i.plugin, undeclared.data(), static_cast<uint32_t>(undeclared.size())));
    i.activate();
    const auto first = requests(static_cast<uint32_t>(product::info::kChannelLayouts[0]));
    CHECK(!configurable->apply_configuration(i.plugin, first.data(), static_cast<uint32_t>(first.size())));
}

TEST(host_bypass_passes_the_input_through_exactly) {
    if (params::kBypassIndex < 0 || !product::info::kAudioInput) return;
    Instance i;
    i.activate(48000.0, 256);
    Audio audio(i.channels());
    InEvents on;
    on.add(0, params::at(static_cast<size_t>(params::kBypassIndex)).id, 1.0);
    OutEvents out;
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    std::vector<std::vector<float>> history;
    double worst = 0.0;
    const int latency = static_cast<int>(static_cast<const clap_plugin_latency_t*>(
        i.plugin->get_extension(i.plugin, CLAP_EXT_LATENCY))->get(i.plugin));
    std::vector<float> input;
    for (int block = 0; block < 40; ++block) {
        std::vector<float> fresh(256);
        for (auto& x : fresh) x = d(rng);
        input.insert(input.end(), fresh.begin(), fresh.end());
        for (int c = 0; c < audio.channels; ++c) audio.data[static_cast<size_t>(c)] = fresh;
        for (auto& c : audio.sidechain) c = fresh;  // a key that would otherwise act
        audio.run(i.plugin, block == 0 ? &on.list : noEvents(), &out);
        if (block >= 4) {  // past the 10 ms crossfade
            for (int k = 0; k < 256; ++k) {
                const int t = block * 256 + k - latency;
                worst = std::max(worst, std::fabs(static_cast<double>(audio.data[0][static_cast<size_t>(k)]) -
                                                  (t >= 0 ? input[static_cast<size_t>(t)] : 0.0)));
            }
            // Aux outputs have no dry signal: bypassed, they are silent.
            for (const auto& bus : audio.aux)
                for (const auto& c : bus)
                    for (float x : c) worst = std::max(worst, static_cast<double>(std::fabs(x)));
        }
    }
    CHECK(worst == 0.0);
}

// --- the editor-to-host protocol ----------------------------------------------------------

TEST(editor_edits_reach_a_stopped_host_through_flush) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    if (!editor) return;
    OutEvents out;
    out.refuse = 0.3;  // a host whose output queue is often full
    std::mt19937 rng(9);
    std::vector<double> last(params::kCount);
    for (size_t k = 0; k < params::kCount; ++k) last[k] = i.value(k);
    for (int step = 0; step < 4000; ++step) {
        const size_t k = rng() % params::kCount;
        if (params::at(k).isOutput()) continue;  // read-only; its own test is below
        const uint32_t id = params::at(k).id;
        const double v = fromNormalized(params::at(k), (rng() % 1000) / 999.0);
        editor->beginEdit(id);
        editor->setValue(id, v);
        last[k] = sanitizeHost(params::at(k), v);
        if (rng() % 2) editor->endEdit(id);
        if (rng() % 3 == 0) i.params->flush(i.plugin, noEvents(), out.fresh());
        if (rng() % 5 == 0) editor->endEdit(id);
        i.pumpMain();
    }
    for (size_t k = 0; k < params::kCount; ++k) editor->endEdit(params::at(k).id);
    out.refuse = 0.0;
    for (int n = 0; n < 4; ++n) {
        i.pumpMain();
        i.params->flush(i.plugin, noEvents(), out.fresh());
    }
    CHECK(out.balanced());
    for (size_t k = 0; k < params::kCount; ++k) {
        if (out.sawValue[k]) CHECK_MSG(out.lastValue[k] == last[k], "%s: host has %g, editor set %g",
                                       params::at(k).name, out.lastValue[k], last[k]);
        CHECK(i.value(k) == last[k]);
    }
    CHECK(i.host.flushRequests > 0);
    CHECK(!i.host.requestFlushOffMain);
}

TEST(editor_edits_and_state_loads_during_playback_are_race_free) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    i.activate(48000.0, 128);
    const std::vector<uint8_t> preset = saveState(i);
    std::atomic<bool> run{true};
    OutEvents out;
    out.refuse = 0.1;
    std::thread audio([&] {
        Audio buffers(i.channels(), 128);
        while (run.load()) {
            for (auto& ch : buffers.data)
                for (auto& x : ch) x = 0.25f;
            buffers.run(i.plugin, noEvents(), &out);
        }
        out.refuse = 0.0;
        for (int n = 0; n < 4; ++n) buffers.run(i.plugin, noEvents(), &out);
    });
    std::mt19937 rng(17);
    for (int step = 0; step < 20000; ++step) {
        const size_t k = rng() % params::kCount;
        const uint32_t id = params::at(k).id;
        if (editor && !params::at(k).isOutput()) {
            editor->beginEdit(id);
            editor->setValue(id, fromNormalized(params::at(k), (rng() % 100) / 99.0));
            editor->endEdit(id);
            TelemetryFrame f;
            editor->popTelemetry(f);
        }
        if (step % 500 == 0) loadState(i, preset);  // a preset recalled while playing
        if (step % 64 == 0) saveState(i);
        i.pumpMain();
    }
    run = false;
    audio.join();
    CHECK(out.balanced());
    CHECK(!i.host.requestFlushOffMain);
}

TEST(telemetry_flows_only_while_metering) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    if (!editor) return;
    i.activate(48000.0, 480);
    Audio audio(i.channels(), 480);
    OutEvents out;
    for (int b = 0; b < 100; ++b) audio.run(i.plugin, noEvents(), &out);  // one second, editor closed
    TelemetryFrame f;
    CHECK(!editor->popTelemetry(f));
    supple_test_set_metering(i.plugin, true);
    for (int b = 0; b < 100; ++b) audio.run(i.plugin, noEvents(), &out);
    int frames = 0;
    while (editor->popTelemetry(f)) ++frames;
    CHECK_MSG(std::abs(frames - static_cast<int>(kTelemetryHz)) <= 1, "%d frames in one second", frames);
}

// --- output parameters ----------------------------------------------------------------------

TEST(output_parameters_reach_the_host_and_stay_out_of_state) {
    if (params::kOutputCount == 0) {
        std::printf("      (no output parameters in this product)\n");
        return;
    }
    Instance i;
    i.activate(48000.0, 480);
    Audio audio(i.channels(), 480);
    // A host that tries to set one is ignored, from the audio thread or the editor.
    InEvents attempt;
    for (uint16_t k : params::kOutputIndices) attempt.add(0, params::at(k).id, fromNormalized(params::at(k), 0.9));
    if (gui::EditorHost* editor = supple_test_editor_host(i.plugin))
        for (uint16_t k : params::kOutputIndices) {
            editor->beginEdit(params::at(k).id);
            editor->setValue(params::at(k).id, fromNormalized(params::at(k), 0.9));
            editor->endEdit(params::at(k).id);
        }
    int events[params::kOutputCount > 0 ? params::kOutputCount : 1]{};
    std::vector<uint8_t> first;
    for (int b = 0; b < 100; ++b) {  // one second of programme
        for (int c = 0; c < audio.channels; ++c)
            for (int f = 0; f < 480; ++f)
                audio.data[static_cast<size_t>(c)][static_cast<size_t>(f)] =
                    0.25f * static_cast<float>(std::sin(0.031 * (b * 480 + f) + c));
        OutEvents block;
        audio.run(i.plugin, b == 0 ? &attempt.list : noEvents(), &block);
        for (size_t n = 0; n < params::kOutputCount; ++n) {
            const size_t k = params::kOutputIndices[n];
            if (!block.sawValue[k]) continue;
            ++events[n];
            // What the host heard is what it reads.
            CHECK_MSG(block.lastValue[k] == i.value(k), "%s: sent %g, reads %g", params::at(k).name,
                      block.lastValue[k], i.value(k));
        }
        if (b == 10) first = saveState(i);
        i.pumpMain();
    }
    for (size_t n = 0; n < params::kOutputCount; ++n) {
        const size_t k = params::kOutputIndices[n];
        const ParamDef& p = params::at(k);
        const double now = i.value(k);
        CHECK_MSG(events[n] <= static_cast<int>(Engine::kOutputHz) + 1, "%s: %d events in a second", p.name, events[n]);
        CHECK_MSG(now >= p.hostMin() && now <= p.hostMax(), "%s reads %g", p.name, now);
        // Once after activation whatever its value, then whenever it moved.
        CHECK_MSG(events[n] >= 1, "%s: the host never heard it", p.name);
        std::printf("      %s: %d values sent in one second, now %g\n", p.name, events[n], now);
    }
    // The meters moved; the state did not.
    CHECK(saveState(i) == first);
    Instance fresh;
    CHECK(loadState(fresh, first));
    for (uint16_t k : params::kOutputIndices) CHECK(fresh.value(k) == hostDefault(params::at(k)));
}

// --- restart protocol ----------------------------------------------------------------------

TEST(a_restart_parameter_asks_once_from_the_main_thread) {
    int index = -1;
    for (size_t k = 0; k < params::kCount; ++k)
        if (params::at(k).is(pflag::kRestart)) index = static_cast<int>(k);
    if (index < 0) return;  // this product has none
    Instance i;
    i.activate();
    Audio audio(i.channels());
    OutEvents out;
    const ParamDef& p = params::at(static_cast<size_t>(index));
    for (int n = 0; n < 3; ++n) {
        InEvents change;
        change.add(0, p.id, fromNormalized(p, n % 2 ? 0.0 : 1.0));
        const int before = i.host.restarts;
        audio.run(i.plugin, &change.list, &out);
        CHECK(i.host.restarts == before);  // never from the audio thread
        i.pumpMain();
    }
    CHECK(i.host.restarts == 1);  // once per activation, however many changes
    i.plugin->stop_processing(i.plugin);
    i.plugin->deactivate(i.plugin);
    i.activate();
    InEvents again;
    again.add(0, p.id, fromNormalized(p, 0.0));  // the loop left it at the top
    audio.run(i.plugin, &again.list, &out);
    i.pumpMain();
    CHECK(i.host.restarts == 2);  // a fresh activation may ask again
}

TEST(the_output_constant_hint_is_cleared) {
    // A host may reuse a buffer descriptor and read sample 0 for a channel
    // whose bit is still set.
    Instance i;
    i.activate();
    if (!product::info::kAudioOutput) return;
    Audio a(i.channels());
    for (auto& o : a.outs) o.constant_mask = ~0ull;
    CHECK(a.run(i.plugin, noEvents(), nullptr) == CLAP_PROCESS_CONTINUE);
    for (const auto& o : a.outs) CHECK(o.constant_mask == 0);
}

TEST(a_tail_that_changes_while_active_is_announced) {
    Instance i;
    i.activate();
    const auto* tail = static_cast<const clap_plugin_tail_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_TAIL));
    CHECK(tail != nullptr);
    if (!tail) return;
    const uint32_t before = tail->get(i.plugin);
    InEvents in;
    for (size_t k = 0; k < params::kCount; ++k)
        if (!params::at(k).is(pflag::kBypass)) in.add(0, params::at(k).id, fromNormalized(params::at(k), 0.9));
    Audio a(i.channels());
    OutEvents out;
    a.run(i.plugin, &in.list, &out);
    if (tail->get(i.plugin) == before) return;  // this product's tail is fixed
    CHECK(i.host.tailChanges.load() >= 1);
}

TEST(a_product_with_note_input_ends_every_note_it_was_given) {
    if (!product::info::kNoteInput) return;
    Instance i;
    i.activate();
    Audio a(i.channels());
    OutEvents out;
    NoteList start;
    start.add(CLAP_EVENT_NOTE_ON, 10, 60, 3, 77);
    start.add(CLAP_EVENT_NOTE_ON, 20, 64, 0, 78);
    a.run(i.plugin, &start.list, &out);
    NoteList stop;
    stop.add(CLAP_EVENT_NOTE_OFF, 0, 60, 3, 77);
    stop.add(CLAP_EVENT_NOTE_CHOKE, 5, -1, -1, 78);
    a.run(i.plugin, &stop.list, &out);
    for (int k = 0; k < 2000 && out.noteEnds.size() < 2; ++k) a.run(i.plugin, noEvents(), &out);
    CHECK_MSG(out.noteEnds.size() == 2, "%zu NOTE_END events", out.noteEnds.size());
    bool saw77 = false, saw78 = false;
    for (const auto& e : out.noteEnds) {
        saw77 |= e.note_id == 77 && e.key == 60 && e.channel == 3 && e.port_index == 0;
        saw78 |= e.note_id == 78 && e.key == 64 && e.channel == 0;
    }
    CHECK(saw77 && saw78);
}

TEST(refused_note_ends_are_retried_at_the_next_blocks_start) {
    if (!product::info::kNoteInput) return;
    Instance i;
    i.activate();
    Audio a(i.channels());
    OutEvents out;
    NoteList start;
    start.add(CLAP_EVENT_NOTE_ON, 10, 60, 0, 77);
    a.run(i.plugin, &start.list, &out);
    out.refuse = 1.0;
    NoteList stop;
    stop.add(CLAP_EVENT_NOTE_CHOKE, 5, 60, 0, 77);
    a.run(i.plugin, &stop.list, &out);
    out.refuse = 0.0;
    a.run(i.plugin, noEvents(), &out);
    CHECK(out.noteEnds.size() == 1);
    if (!out.noteEnds.empty()) CHECK(out.noteEnds[0].header.time == 0 && out.noteEnds[0].note_id == 77);
    a.run(i.plugin, noEvents(), &out);
    CHECK(out.noteEnds.size() == 1);
}

TEST(the_editors_keyboard_wakes_a_sleeping_host) {
    Instance i;
    auto* editor = supple_test_editor_host(i.plugin);
    if (!editor || !product::info::kNoteInput) return;
    editor->playNote(60, 0.5, true);
    CHECK(i.host.processRequests > 0);
}

TEST(the_editors_keyboard_plays_through_the_plugin_and_ends_its_notes) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    if (!editor || !product::info::kNoteInput) {
        std::printf("      (no editor, or no note input)\n");
        return;
    }
    i.activate(48000.0, 480);
    Audio a(i.channels(), 480);
    OutEvents out;
    supple_test_set_metering(i.plugin, true);
    editor->playNote(62, 0.8, true);
    for (int b = 0; b < 10; ++b) a.run(i.plugin, noEvents(), &out);
    TelemetryFrame f{}, next{};
    CHECK(editor->popTelemetry(f));
    while (editor->popTelemetry(next)) f = next;
    CHECK(f.keys[0] == uint64_t{1} << 62);  // held, as the keyboard will draw it
    editor->playNote(62, 0.0, false);
    for (int k = 0; k < 2000 && out.noteEnds.empty(); ++k) a.run(i.plugin, noEvents(), &out);
    // The processor ended the note it was played, as it ends a host's.
    CHECK(!out.noteEnds.empty() && out.noteEnds[0].key == 62 && out.noteEnds[0].port_index == 0);
}

// --- notes ------------------------------------------------------------------------------------

TEST(note_ports_are_as_declared) {
    Instance i;
    const auto* notes = static_cast<const clap_plugin_note_ports_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_NOTE_PORTS));
    CHECK((notes != nullptr) == (product::info::kNoteInput || product::info::kNoteOutput));
    if (!notes) return;
    CHECK(notes->count(i.plugin, true) == (product::info::kNoteInput ? 1u : 0u));
    CHECK(notes->count(i.plugin, false) == (product::info::kNoteOutput ? 1u : 0u));
    for (int dir = 0; dir < 2; ++dir)
        for (uint32_t k = 0; k < notes->count(i.plugin, dir == 0); ++k) {
            clap_note_port_info_t info{};
            CHECK(notes->get(i.plugin, k, dir == 0, &info));
            CHECK((info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI));
        }
}

namespace {

// Every kind of note-port event a host sends, mixed, at rising frames.
struct MixedEvents {
    std::vector<std::vector<uint8_t>> storage;
    std::vector<const clap_event_header_t*> list;
    clap_input_events_t events{this, [](const clap_input_events_t* l) {
                                    return static_cast<uint32_t>(static_cast<const MixedEvents*>(l->ctx)->list.size());
                                },
                                [](const clap_input_events_t* l, uint32_t k) {
                                    return static_cast<const MixedEvents*>(l->ctx)->list[k];
                                }};
    template <class E>
    E& add(uint16_t type, uint32_t time) {
        storage.emplace_back(sizeof(E), uint8_t{0});
        auto* e = reinterpret_cast<E*>(storage.back().data());
        e->header.size = sizeof(E);
        e->header.time = time;
        e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e->header.type = type;
        return *e;
    }
    void finish() {
        list.clear();
        for (auto& b : storage) list.push_back(reinterpret_cast<const clap_event_header_t*>(b.data()));
    }
};

}  // namespace

TEST(truncated_parameter_events_are_ignored) {
    Instance i;
    i.activate();
    size_t index = 0;
    while (index < params::kCount && params::at(index).isOutput()) ++index;
    if (index == params::kCount) return;
    const auto& p = params::at(index);
    const double before = i.value(index);
    MixedEvents events;
    auto& e = events.add<clap_event_param_value_t>(CLAP_EVENT_PARAM_VALUE, 0);
    e.param_id = p.id;
    e.value = before == p.hostMax() ? p.hostMin() : p.hostMax();
    e.header.size = sizeof(clap_event_header_t);
    events.finish();
    Audio audio(i.channels());
    audio.run(i.plugin, &events.events, nullptr);
    CHECK(i.value(index) == before);
}

TEST(polyphonic_modulation_is_advertised_to_the_host) {
    Instance i;
    const auto* extension = static_cast<const clap_plugin_params_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_PARAMS));
    constexpr uint32_t poly = CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
                              CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_PORT;
    for (size_t k = 0; k < params::kCount; ++k) {
        clap_param_info_t info{};
        CHECK(extension->get_info(i.plugin, static_cast<uint32_t>(k), &info));
        CHECK_MSG((info.flags & poly) == (params::at(k).is(pflag::kPolyMod) ? poly : 0u), "%s", params::at(k).name);
    }
    if (!params::kAnyPolyMod) std::puts("      (no polyphonic modulation in this product)");
}

TEST(midi_expressions_and_note_modulation_are_taken_in_any_mix) {
    if (!product::info::kNoteInput) {
        std::printf("      (no note input in this product)\n");
        return;
    }
    Instance i;
    i.activate(48000.0, 256);
    Audio a(i.channels());
    OutEvents out;
    for (int block = 0; block < 200; ++block) {
        MixedEvents m;
        const int16_t key = static_cast<int16_t>(48 + block % 24);
        auto& on = m.add<clap_event_note_t>(CLAP_EVENT_NOTE_ON, 3);
        on.note_id = block;
        on.port_index = 0;
        on.channel = static_cast<int16_t>(block % 16);
        on.key = key;
        on.velocity = 0.7;
        auto& cc = m.add<clap_event_midi_t>(CLAP_EVENT_MIDI, 10);
        cc.data[0] = static_cast<uint8_t>(0xB0 | (block % 16));
        cc.data[1] = 1;
        cc.data[2] = static_cast<uint8_t>(block % 128);
        auto& bend = m.add<clap_event_midi_t>(CLAP_EVENT_MIDI, 20);
        bend.data[0] = static_cast<uint8_t>(0xE0 | (block % 16));
        bend.data[1] = 0;
        bend.data[2] = static_cast<uint8_t>((block * 7) % 128);
        auto& x = m.add<clap_event_note_expression_t>(CLAP_EVENT_NOTE_EXPRESSION, 30);
        x.expression_id = block % 7;
        x.note_id = block;
        x.port_index = 0;
        x.channel = static_cast<int16_t>(block % 16);
        x.key = key;
        x.value = (block % 7) == CLAP_NOTE_EXPRESSION_TUNING ? 1.5 : 0.6;
        for (size_t k = 0; k < params::kCount; ++k) {
            if (!params::at(k).is(pflag::kPolyMod)) continue;
            auto& mod = m.add<clap_event_param_mod_t>(CLAP_EVENT_PARAM_MOD, 40);
            mod.param_id = params::at(k).id;
            mod.note_id = block;
            mod.port_index = -1;
            mod.channel = -1;
            mod.key = -1;
            mod.amount = 0.25 * (params::at(k).hostMax() - params::at(k).hostMin());
        }
        if (block > 4) {
            auto& off = m.add<clap_event_note_t>(CLAP_EVENT_NOTE_OFF, 200);
            off.note_id = block - 4;
            off.port_index = 0;
            off.channel = static_cast<int16_t>((block - 4) % 16);
            off.key = static_cast<int16_t>(48 + (block - 4) % 24);
        }
        m.finish();
        CHECK(a.run(i.plugin, &m.events, &out) == CLAP_PROCESS_CONTINUE);
    }
    for (const auto& c : a.data)
        for (float v : c) CHECK(std::isfinite(v));
    CHECK(!out.outOfOrder);
}

TEST(voice_info_reports_the_processors_voices) {
    Instance i;
    const auto* voices = static_cast<const clap_plugin_voice_info_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_VOICE_INFO));
    const bool declared = product::info::kNoteInput && product::Processor::kVoices > 0;
    CHECK((voices != nullptr) == declared);
    if (!voices) return;
    i.activate();
    clap_voice_info_t info{};
    CHECK(voices->get(i.plugin, &info));
    CHECK(info.voice_capacity == static_cast<uint32_t>(product::Processor::kVoices));
    CHECK(info.voice_count >= 1 && info.voice_count <= info.voice_capacity);
    CHECK(((info.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) != 0) == product::Processor::kOverlappingNotes);
}

// --- what the host says, and what the plugin tells it ----------------------------------------

template <class T>
const T* extension(const Instance& i, const char* id) {
    return static_cast<const T*>(i.plugin->get_extension(i.plugin, id));
}

// A parameter a host could set: not an output, not bypass.
size_t settable() {
    for (size_t k = 0; k < params::kCount; ++k)
        if (!params::at(k).isOutput() && !params::at(k).is(pflag::kBypass)) return k;
    return 0;
}

TEST(each_call_on_a_thread_clap_does_not_allow_is_logged_once) {
    if (!kThreadChecks) {
        std::printf("      (a release build checks no threads)\n");
        return;
    }
    Instance i([](Host& h) { h.threadCheck = true; });
    // A whole session where every call is on its thread: nothing to say.
    CHECK(i.plugin->activate(i.plugin, 48000.0, 1, 256));
    i.active = true;
    Audio a(i.channels());
    OutEvents out;
    {
        AudioScope audio;
        i.plugin->start_processing(i.plugin);
        for (int k = 0; k < 4; ++k) a.run(i.plugin, noEvents(), &out);
        i.params->flush(i.plugin, noEvents(), out.fresh());
        i.plugin->reset(i.plugin);
        i.plugin->stop_processing(i.plugin);
    }
    saveState(i);
    i.pumpMain();
    CHECK(supple_test_wrong_threads(i.plugin) == 0 && i.host.logged.empty());

    // A main-thread call from another thread, twice: one line, naming it.
    for (int k = 0; k < 2; ++k) {
        std::thread other([&] { i.params->count(i.plugin); });
        other.join();
        i.pumpMain();
    }
    CHECK(supple_test_wrong_threads(i.plugin) != 0);
    CHECK(i.host.logged.size() == 1 && i.host.logged[0].first == CLAP_LOG_HOST_MISBEHAVING &&
          i.host.loggedContaining("params.count") == 1);
    // And process() on the main thread.
    a.run(i.plugin, noEvents(), &out);
    i.pumpMain();
    CHECK(i.host.logged.size() == 2 && i.host.loggedContaining("process") == 1);
}

TEST(trouble_on_the_audio_thread_is_logged_from_the_main_thread_once) {
    if (!product::info::kAudioOutput) {
        std::printf("      (no audio in this product)\n");
        return;
    }
    Instance i;
    i.activate();
    Audio a(i.channels());
    a.outs[0].channel_count = static_cast<uint32_t>(i.channels() - 1);  // narrower than the layout
    for (int k = 0; k < 3; ++k) {
        CHECK(a.run(i.plugin, noEvents(), nullptr) == CLAP_PROCESS_ERROR);
        i.pumpMain();
    }
    CHECK(supple_test_reports(i.plugin) != 0);
    CHECK(i.host.logged.size() == 1 && i.host.logged[0].first == CLAP_LOG_HOST_MISBEHAVING);
    // Damaged state is refused, and said so.
    std::vector<uint8_t> bytes = saveState(i);
    bytes.resize(bytes.size() / 2);
    ByteReader r{&bytes};
    CHECK(!i.state->load(i.plugin, &r.stream));
    CHECK(i.host.logged.size() == 2 && i.host.logged[1].first == CLAP_LOG_WARNING);
}

TEST(an_offline_mode_hears_the_hosts_render_mode_and_no_other_product_claims_one) {
    Instance i;
    const auto* render = extension<clap_plugin_render_t>(i, CLAP_EXT_RENDER);
    CHECK((render != nullptr) == product::Processor::kOfflineMode);
    if (!render) {
        std::printf("      (no offline mode in this product)\n");
        return;
    }
    CHECK(!render->has_hard_realtime_requirement(i.plugin));
    CHECK(render->set(i.plugin, CLAP_RENDER_OFFLINE) && supple_test_offline(i.plugin));
    CHECK(render->set(i.plugin, CLAP_RENDER_REALTIME) && !supple_test_offline(i.plugin));
    CHECK(!render->set(i.plugin, 7) && !supple_test_offline(i.plugin));
}

TEST(a_hosts_context_menu_offers_reset_as_one_whole_edit) {
    Instance i;
    i.activate();
    const auto* menu = extension<clap_plugin_context_menu_t>(i, CLAP_EXT_CONTEXT_MENU);
    CHECK(menu != nullptr);
    if (!menu) return;
    struct Collected {
        std::vector<std::string> labels;
        std::vector<clap_id> actions;
    } got;
    clap_context_menu_builder_t builder{&got,
                                        [](const clap_context_menu_builder_t* b, clap_context_menu_item_kind_t kind,
                                           const void* data) {
                                            auto& c = *static_cast<Collected*>(b->ctx);
                                            if (kind != CLAP_CONTEXT_MENU_ITEM_ENTRY) return false;
                                            const auto* e = static_cast<const clap_context_menu_entry_t*>(data);
                                            c.labels.push_back(e->label);
                                            c.actions.push_back(e->action_id);
                                            return true;
                                        },
                                        [](const clap_context_menu_builder_t*, clap_context_menu_item_kind_t) {
                                            return true;
                                        }};
    const size_t k = settable();
    const ParamDef& p = params::at(k);
    const clap_context_menu_target_t target{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, p.id};
    CHECK(menu->populate(i.plugin, &target, &builder) && got.labels.size() == 1 && got.labels[0] == "Reset to Default");
    const clap_context_menu_target_t global{CLAP_CONTEXT_MENU_TARGET_KIND_GLOBAL, 0};
    CHECK(menu->populate(i.plugin, &global, &builder) && got.labels.size() == 1);
    if (got.actions.empty()) return;

    const double away = fromNormalized(p, 0.37) != hostDefault(p) ? fromNormalized(p, 0.37) : fromNormalized(p, 0.71);
    InEvents move;
    move.add(0, p.id, away);
    OutEvents out;
    i.params->flush(i.plugin, &move.list, out.fresh());
    CHECK(i.value(k) == away);
    CHECK(menu->perform(i.plugin, &target, got.actions[0]) && i.value(k) == hostDefault(p));
    CHECK(!menu->perform(i.plugin, &global, got.actions[0]));
    i.params->flush(i.plugin, noEvents(), out.fresh());
    CHECK(out.sawValue[k] && out.lastValue[k] == hostDefault(p) && out.balanced());
}

TEST(the_editor_gets_the_hosts_context_menu_flattened) {
    Instance i([](Host& h) { h.contextMenu = true; });
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    if (!editor) {
        std::printf("      (no editor in this build)\n");
        return;
    }
    const uint32_t id = params::at(settable()).id;
    gui::HostMenu m;
    CHECK(editor->hostMenu(id, m));
    using Kind = gui::MenuItem::Kind;
    CHECK(m.items.size() == 6 && m.actions.size() == 6);
    if (m.items.size() != 6 || m.actions.size() != 6) return;
    CHECK(m.items[0].label == "Automate" && m.items[0].kind == Kind::Item && m.actions[0] == 10);
    CHECK(m.items[1].label == "MIDI" && m.items[1].kind == Kind::Header);
    CHECK(m.items[2].label == "Learn" && m.items[2].checked && m.actions[2] == 11);
    CHECK(m.items[3].kind == Kind::Separator && m.items[4].kind == Kind::Separator);
    CHECK(m.items[5].label == "Title" && m.items[5].kind == Kind::Header);
    CHECK(editor->performHostMenu(id, 11) && i.host.performedParam == id && i.host.performedAction == 11);
    CHECK(!editor->hostPopup(id, 10.0f, 10.0f));  // no window, and this host cannot
}

TEST(the_editor_knows_the_track_and_hears_when_it_changes) {
    Instance i;
    gui::EditorHost* editor = supple_test_editor_host(i.plugin);
    if (!editor) {
        std::printf("      (no editor in this build)\n");
        return;
    }
    gui::TrackInfo t = editor->track();
    CHECK(t.name == "Lead" && t.hasColour && t.alpha == 255 && t.red == 30 && t.green == 160 && t.blue == 90);
    CHECK(t.bus && !t.master && !t.returnTrack);
    const uint64_t before = editor->trackVersion();
    i.host.trackName = "Bass";
    i.host.trackColour = false;
    const auto* track = extension<clap_plugin_track_info_t>(i, CLAP_EXT_TRACK_INFO);
    CHECK(track != nullptr);
    if (!track) return;
    track->changed(i.plugin);
    t = editor->track();
    CHECK(editor->trackVersion() != before && t.name == "Bass" && !t.hasColour);
}

TEST(note_names_are_the_processors_and_the_host_hears_each_change) {
    Instance i;
    const auto* names = extension<clap_plugin_note_name_t>(i, CLAP_EXT_NOTE_NAME);
    if (!names) {
        std::printf("      (no note names in this product)\n");
        return;
    }
    CHECK(product::info::kNoteInput);
    // What a host reads, against the processor's own list at these values.
    auto expected = [&] {
        ParamValues v;
        for (size_t k = 0; k < params::kCount; ++k) v.setIndex(k, toPlain(params::at(k), i.value(k)));
        NoteNames n;
        noteNamesOf<product::Processor>(v, n);
        return n;
    };
    auto read = [&] {
        NoteNames n;
        const uint32_t count = names->count(i.plugin);
        for (uint32_t k = 0; k < count; ++k) {
            clap_note_name_t nn{};
            CHECK(names->get(i.plugin, k, &nn));
            n.add(nn.key, nn.name, nn.channel, nn.port);
        }
        return n;
    };
    NoteNames seen = read();
    CHECK(seen.size() > 0 && seen == expected());
    clap_note_name_t none{};
    CHECK(!names->get(i.plugin, static_cast<uint32_t>(seen.size()), &none));

    // Move each parameter in turn, as automation: whenever the names change,
    // the host hears so (after a main-thread callback), and reads them again.
    i.activate();
    Audio a(i.channels());
    OutEvents out;
    for (size_t k = 0; k < params::kCount; ++k) {
        const ParamDef& p = params::at(k);
        if (p.isOutput()) continue;
        for (double normal : {0.37, 0.71, 0.0}) {
            InEvents move;
            move.add(0, p.id, fromNormalized(p, normal));
            const int told = i.host.noteNamesChanged;
            a.run(i.plugin, &move.list, &out);
            i.pumpMain();
            const bool changed = !(expected() == seen);
            CHECK_MSG(changed == (i.host.noteNamesChanged > told), "%s at %g: names %s, host %s", p.name, normal,
                      changed ? "changed" : "unchanged", i.host.noteNamesChanged > told ? "told" : "not told");
            if (i.host.noteNamesChanged > told) seen = read();
        }
    }
}

TEST(the_hosts_tuning_retunes_and_its_unmapped_keys_end_at_once) {
    Instance i([](Host& h) { h.tuning = true; });
    const auto* ext = extension<clap_plugin_tuning_t>(i, CLAP_EXT_TUNING);
    CHECK((ext != nullptr) == product::info::kTuning);
    if (!ext) {
        std::printf("      (no tuning in this product)\n");
        return;
    }
    i.activate();
    CHECK(supple_test_retuning(i.plugin, 60, 0) == 0.0);  // nothing chosen yet
    Audio a(i.channels());
    OutEvents out;
    MixedEvents m;
    auto& choose = m.add<clap_event_tuning_t>(0, 0);
    choose.header.space_id = kTuningSpace;
    choose.port_index = -1;
    choose.channel = -1;
    choose.tunning_id = 7;
    for (int16_t key : {int16_t{60}, int16_t{61}}) {
        auto& on = m.add<clap_event_note_t>(CLAP_EVENT_NOTE_ON, static_cast<uint32_t>(key - 50));
        on.note_id = key;
        on.port_index = 0;
        on.channel = 0;
        on.key = key;
        on.velocity = 0.8;
    }
    m.finish();
    CHECK(a.run(i.plugin, &m.events, &out) == CLAP_PROCESS_CONTINUE);
    CHECK(out.noteEnds.size() == 1 && out.noteEnds[0].key == 61 && out.noteEnds[0].header.time == 11);
    CHECK(supple_test_retuning(i.plugin, 60, 0) == 0.5);
}

SUPPLE_TEST_MAIN
