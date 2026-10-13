// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The CLAP plugin. clap-wrapper builds the VST3 and AUv2 from this same
// object code, so every format has the same parameters, state and sound.
//
// This file translates; it does not decide. Audio, parameters, bypass and
// telemetry belong to Engine (core/Engine.h); the editor's edits to
// EditBridge; text to core/Param.h; state to core/StateFormat.h. What is left
// is the CLAP surface and the threading contract around it:
//
//   main thread   init, activate, state, presets, params info/text, gui,
//                 timers, on_main_thread. Writes only the ParamStore and the
//                 PropertyStore.
//   audio thread  process, reset, and params.flush while active. The only
//                 thread that touches the processor while it is active.
//
// Host-facing requests made from the audio thread (restart, flush retries,
// what goes in the host's log) go through request_callback and are made from
// on_main_thread. A debug build checks every entry point's thread against
// CLAP's rules and logs a host that breaks them.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/draft/tuning.h>
#include <clapwrapper/vst3.h>

#if SUPPLE_TUNING_MTS
#include <libMTSClient.h>
#endif

#include "core/Catalog.h"
#include "core/Definition.h"
#include "core/Engine.h"
#include "core/PresetLibrary.h"
#include "core/Realtime.h"
#include "core/StateFormat.h"
#include "plugin/EditBridge.h"
#include "plugin/Entry.h"
#include "plugin/Layer.h"
#include "plugin/PresetDiscovery.h"
#include "plugin/TestHooks.h"
#include "ProductInfo.h"
#if !SUPPLE_PRODUCT_RUNTIME
#include "core/ParamTable.h"
#endif

#if SUPPLE_WITH_GUI
#include "gui/EditorHost.h"
#include "gui/Gui.h"
#endif

namespace supple::plugin {
namespace {

using Processor = product::Processor;

#if SUPPLE_WITH_GUI
constexpr uint32_t kTimerMs = 16;  // ~60 Hz editor ticks
#endif
#if SUPPLE_PRODUCT_LAYER
constexpr uint32_t kLayerTimerMs = 33;  // ~30 Hz layer ticks
#endif

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

// What the processor asked for beyond the basics.
constexpr bool kRender = Processor::kOfflineMode;

#if !SUPPLE_PRODUCT_RUNTIME
// A product with tables is checked as it compiles; a run-time product's
// definitions by Definition::problems() as they are made.
static_assert(product::info::kNoteInput || !NamesNotes<Processor>,
              "noteNames() names keys for notes the plugin takes: it needs note input");
static_assert(!product::info::kTuning || product::info::kNoteInput, "PRODUCT_TUNING needs note input");
static_assert(!params::kAnyPolyMod || product::info::kNoteInput,
              "polyModulatable() needs note input: an instrument, a note effect, or PRODUCT_NOTE_INPUT ON");
#endif

// --- events ---------------------------------------------------------------------

// Feeds the engine CLAP's input events in order, translated and filtered.
// Parameter values are taken as global: nothing here declares per-note
// automation, so a host may not send any. Modulation addressed to notes goes
// to the processor's noteModulation() for a parameter marked
// polyModulatable(), and is dropped for any other. Nor may a host set an
// output parameter, which is read-only. Notes, MIDI and expressions reach a
// product with note input; the rest drop them.
class ClapEvents {
  public:
    // `tuningSpace`: where the host puts CLAP tuning events, for a product
    // with PRODUCT_TUNING; UINT16_MAX for none. `notes`: the plugin takes
    // notes and MIDI.
    ClapEvents(const ParamTable& params, bool notes, const clap_input_events_t* in, uint16_t tuningSpace = UINT16_MAX)
        : params_(params), notes_(notes), in_(in), count_(in ? in->size(in) : 0), tuningSpace_(tuningSpace) {}

    bool next(EngineEvent& e) {
        while (index_ < count_) {
            const clap_event_header_t* h = in_->get(in_, index_++);
            if (!h || h->size < sizeof(clap_event_header_t)) continue;
            if (h->space_id == tuningSpace_ && h->space_id != CLAP_CORE_EVENT_SPACE_ID) {
                if (h->size < sizeof(clap_event_tuning_t)) continue;
                const auto* t = reinterpret_cast<const clap_event_tuning_t*>(h);
                e = EngineEvent{};
                e.time = h->time;
                e.type = EngineEvent::Type::Tuning;
                e.address.port = t->port_index;
                e.address.channel = t->channel;
                e.tuning = t->tunning_id;
                return true;
            }
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            e = EngineEvent{};
            e.time = h->time;
            switch (h->type) {
                case CLAP_EVENT_PARAM_VALUE: {
                    if (h->size < sizeof(clap_event_param_value_t)) continue;
                    const auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
                    e.index = params_.indexOf(v->param_id);
                    if (e.index < 0 || params_.at(static_cast<size_t>(e.index)).isOutput()) continue;
                    e.type = EngineEvent::Type::Value;
                    e.value = v->value;
                    return true;
                }
                case CLAP_EVENT_PARAM_MOD: {
                    if (h->size < sizeof(clap_event_param_mod_t)) continue;
                    const auto* m = reinterpret_cast<const clap_event_param_mod_t*>(h);
                    e.index = params_.indexOf(m->param_id);
                    if (e.index < 0 || params_.at(static_cast<size_t>(e.index)).isOutput()) continue;
                    e.value = m->amount;
                    if (m->note_id == -1 && m->port_index == -1 && m->channel == -1 && m->key == -1) {
                        e.type = EngineEvent::Type::Modulation;
                        return true;
                    }
                    if (!params_.at(static_cast<size_t>(e.index)).is(pflag::kPolyMod)) continue;
                    e.type = EngineEvent::Type::NoteModulation;
                    e.address = NoteAddress{m->note_id, m->port_index, m->channel, m->key};
                    return true;
                }
                case CLAP_EVENT_NOTE_EXPRESSION: {
                    if (h->size < sizeof(clap_event_note_expression_t)) continue;
                    if (!notes_) continue;
                    const auto* x = reinterpret_cast<const clap_event_note_expression_t*>(h);
                    if (x->expression_id < CLAP_NOTE_EXPRESSION_VOLUME || x->expression_id > CLAP_NOTE_EXPRESSION_PRESSURE)
                        continue;
                    e.type = EngineEvent::Type::Expression;
                    e.expression = static_cast<NoteExpression::Type>(x->expression_id);
                    e.address = NoteAddress{x->note_id, x->port_index, x->channel, x->key};
                    e.value = x->value;
                    return true;
                }
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE: {
                    if (h->size < sizeof(clap_event_note_t)) continue;
                    if (!notes_) continue;
                    const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
                    e.type = EngineEvent::Type::Note;
                    e.note.type = h->type == CLAP_EVENT_NOTE_ON    ? NoteEvent::Type::On
                                  : h->type == CLAP_EVENT_NOTE_OFF ? NoteEvent::Type::Off
                                                                   : NoteEvent::Type::Choke;
                    e.note.key = n->key;
                    e.note.channel = n->channel;
                    e.note.port = n->port_index;
                    e.note.noteId = n->note_id;
                    e.note.velocity = std::clamp(n->velocity, 0.0, 1.0);
                    return true;
                }
                case CLAP_EVENT_MIDI: {
                    if (h->size < sizeof(clap_event_midi_t)) continue;
                    if (!notes_) continue;
                    const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                    const uint8_t status = m->data[0] & 0xF0;
                    if (status >= 0xA0 && status <= 0xE0) {
                        // Controllers, pitch bend, pressure, program changes.
                        e.type = EngineEvent::Type::Midi;
                        e.midi = MidiMessage::make(status, m->data[0] & 0x0F, m->data[1], m->data[2]);
                        e.midi.port = static_cast<int16_t>(m->port_index);
                        return true;
                    }
                    if (status != 0x90 && status != 0x80) continue;
                    e.type = EngineEvent::Type::Note;
                    e.note.type = status == 0x90 && m->data[2] > 0 ? NoteEvent::Type::On : NoteEvent::Type::Off;
                    e.note.key = static_cast<int16_t>(m->data[1] & 0x7F);
                    e.note.channel = static_cast<int16_t>(m->data[0] & 0x0F);
                    e.note.port = static_cast<int16_t>(m->port_index);
                    e.note.velocity = (m->data[2] & 0x7F) / 127.0;
                    return true;
                }
                default: continue;
            }
        }
        return false;
    }

  private:
    const ParamTable& params_;
    bool notes_;
    const clap_input_events_t* in_;
    uint32_t count_;
    uint32_t index_ = 0;
    uint16_t tuningSpace_;
};

// --- diagnostics --------------------------------------------------------------------

// Every entry point whose thread CLAP fixes, for the debug build's checks.
enum class Site : uint8_t {
    Init, Destroy, Activate, Deactivate, StartProcessing, StopProcessing, Reset, Process, OnMainThread,
    ParamsCount, ParamsInfo, ParamsValue, ParamsToText, ParamsFromText, ParamsFlush,
    StateSave, StateLoad, PortsConfig, Latency, PresetLoad,
    GuiCreate, GuiDestroy, GuiSize, GuiParent, GuiShow, GuiHide, Timer,
    Render, TrackInfo, ContextMenu, NoteNames, Tuning, kCount
};
constexpr const char* kSiteNames[] = {
    "init", "destroy", "activate", "deactivate", "start_processing", "stop_processing", "reset", "process",
    "on_main_thread", "params.count", "params.get_info", "params.get_value", "params.value_to_text",
    "params.text_to_value", "params.flush", "state.save", "state.load", "audio-ports-config", "latency.get",
    "preset-load", "gui.create", "gui.destroy", "gui size", "gui.set_parent", "gui.show", "gui.hide",
    "timer-support.on_timer", "render", "track-info.changed", "context-menu", "note-name", "tuning.changed"};
static_assert(std::size(kSiteNames) == static_cast<size_t>(Site::kCount));
enum class Thread : uint8_t { Main, Audio, AudioWhileActive };

// What the audio thread finds wrong, logged from the main thread, once each
// per instance.
enum Report : uint32_t {
    kNonFinite = 1u << 0,     // the guard silenced a NaN or infinity
    kNotesDropped = 1u << 1,  // more notes in one block than NoteOutput holds
    kBadBuses = 1u << 2,      // process() without the buses the plugin declared
};

// --- tuning -------------------------------------------------------------------------

// Where retuning() comes from (PRODUCT_TUNING): an MTS-ESP master while one
// is running, else the tuning the host chose for the note's channel (CLAP
// tuning), else none. Only the audio thread calls it; the host's tuning is
// valid only during process(), which is the only time a processor asks.
class PluginTuning final : public TuningSource {
  public:
    void attach(const clap_host_t* host, const clap_host_tuning_t* tuning) {
        host_ = host;
        tuning_ = tuning && tuning->get_relative && tuning->should_play ? tuning : nullptr;
#if SUPPLE_TUNING_MTS
        if (!mts_) mts_ = MTS_RegisterClient();
#endif
    }
    void detach() {
#if SUPPLE_TUNING_MTS
        if (mts_) MTS_DeregisterClient(mts_);
        mts_ = nullptr;
#endif
    }

    double retuning(int key, int channel, uint32_t frame) override {
        key = std::clamp(key, 0, 127);
#if SUPPLE_TUNING_MTS
        if (mts_ && MTS_HasMaster(mts_))
            return MTS_RetuningInSemitones(mts_, static_cast<char>(key), static_cast<signed char>(channel));
#endif
        const clap_id id = chosen(channel);
        return tuning_ && id != CLAP_INVALID_ID ? tuning_->get_relative(host_, id, channel, key, frame) : 0.0;
    }
    bool plays(int key, int channel) override {
        key = std::clamp(key, 0, 127);
#if SUPPLE_TUNING_MTS
        if (mts_ && MTS_HasMaster(mts_))
            return !MTS_ShouldFilterNote(mts_, static_cast<char>(key), static_cast<signed char>(channel));
#endif
        const clap_id id = chosen(channel);
        return !tuning_ || id == CLAP_INVALID_ID || tuning_->should_play(host_, id, channel, key);
    }
    // The plugin has one note port, so a tuning for any other is not ours.
    void select(int port, int channel, uint32_t id) override {
        if (port > 0) return;
        if (channel < 0 || channel > 15) {
            for (clap_id& c : channels_) c = id;
        } else {
            channels_[channel] = id;
        }
    }

  private:
    clap_id chosen(int channel) const { return channels_[channel >= 0 && channel < 16 ? channel : 0]; }

    const clap_host_t* host_ = nullptr;
    const clap_host_tuning_t* tuning_ = nullptr;
    clap_id channels_[16] = {CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID,
                             CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID,
                             CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID,
                             CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID};
#if SUPPLE_TUNING_MTS
    MTSClient* mts_ = nullptr;
#endif
};

// --- the plugin -------------------------------------------------------------------

struct Plugin final : StateAccess, Instance
#if SUPPLE_WITH_GUI
    , gui::EditorHost
#endif
{
    clap_plugin_t clap{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_latency_t* hostLatency = nullptr;
    const clap_host_timer_support_t* hostTimer = nullptr;
    const clap_host_gui_t* hostGui = nullptr;
    const clap_host_tail_t* hostTail = nullptr;
    const clap_host_voice_info_t* hostVoiceInfo = nullptr;
    const clap_host_state_t* hostState = nullptr;
    const clap_host_preset_load_t* hostPresetLoad = nullptr;
    const clap_host_log_t* hostLog = nullptr;
    const clap_host_thread_check_t* hostThreadCheck = nullptr;
    const clap_host_context_menu_t* hostContextMenu = nullptr;
    const clap_host_track_info_t* hostTrackInfo = nullptr;
    const clap_host_note_name_t* hostNoteName = nullptr;
    const clap_host_audio_ports_t* hostAudioPorts = nullptr;
    const clap_host_note_ports_t* hostNotePorts = nullptr;
    uint16_t tuningSpace = UINT16_MAX;  // where the host's tuning events are, if anywhere
    // The host creates the plugin on its main thread; without thread-check,
    // that is how a debug build knows it.
    std::thread::id mainThread = std::this_thread::get_id();

    // What this instance is. Everything below follows from it.
    DefinitionPtr definition;
    uint32_t magic;  // the state tag
    std::unique_ptr<Engine> engine;
    std::unique_ptr<EditBridge> edits;
    int channels;  // the main bus width: the selected layout's
    bool active = false;
    int reportedLatency = -1;         // main thread: what the host was last told
    bool restartOutstanding = false;  // main thread
    double editorScale = 1.0;         // main thread: survives editor close and state
    // Each output parameter's value as the host last heard it, by event; its
    // get_value reports the same, so the host's two views never disagree.
    // (The editor reads the store, which follows every block.) Written on
    // the audio thread.
    std::unique_ptr<std::atomic<double>[]> heardOutputs;  // by place in table().outputs()
    bool resendOutputs = true;  // audio thread: after activation, tell the host every value once

    // The preset library (main thread), for a product with presets.
    std::unique_ptr<PresetLibrary> presets;
    PluginTuning tuning;  // PRODUCT_TUNING
    NoteNames noteNameList;  // main thread: the note names as the host last read them

    // The product's layer (plugin/Layer.h), if it has one, and its timer.
    std::unique_ptr<Layer> layer;
    clap_id layerTimerId = CLAP_INVALID_ID;
    std::atomic<bool> layerCallback{false};
    // What redefine() asked for, applied at the next deactivation.
    DefinitionPtr pendingDefinition;
    bool applyRestart = false;  // main thread: an apply changed a restart parameter

    explicit Plugin(DefinitionPtr d)
        : definition(std::move(d)),
          magic(state::fourcc(definition->info().stateTag.c_str())),
          engine(std::make_unique<Engine>(definition)),
          edits(std::make_unique<EditBridge>(definition->params().count())),
          channels(definition->info().layouts.empty() ? 0 : definition->info().layouts[0].channels),
          heardOutputs(std::make_unique<std::atomic<double>[]>(std::max<size_t>(1, table().outputs().size()))) {
        for (size_t k = 0; k < table().outputs().size(); ++k)
            heardOutputs[k].store(hostDefault(table().at(table().outputs()[k])), std::memory_order_relaxed);
    }

    const PluginInfo& info() const { return definition->info(); }
    const ParamTable& table() const { return definition->params(); }
    const PropertyList& propertyList() const { return definition->properties(); }
    // Note names: a processor that has them, for notes the plugin takes.
    bool namesNotes() const { return NamesNotes<Processor> && info().takesNotes(); }
    // Voices, for hosts that plan around them (CLAP voice-info).
    int voiceCapacity() const { return info().voices > 0 ? info().voices : Processor::kVoices; }
    // --- Instance: what the layer sees (main thread) --------------------------------

    const clap_plugin_t* clapPlugin() const override { return &clap; }
    const clap_host_t* clapHost() const override { return host; }
    const void* hostExtension(const char* id) const override {
        return host && host->get_extension ? host->get_extension(host, id) : nullptr;
    }
    const DefinitionPtr& currentDefinition() const override { return definition; }
    bool isActive() const override { return active; }
    state::Snapshot snapshot() const override {
        engine->properties().collect();
        state::Snapshot s(*definition);
        for (size_t i = 0; i < table().count(); ++i) s.host[i] = engine->store().get(i);
        for (size_t i = 0; i < propertyList().count(); ++i) {
            s.property[i] = engine->properties().get(i);
            s.hasProperty[i] = true;
        }
        return s;
    }
    void* processor() override { return &engine->processor(); }
    void requestLayerCallback() override {
        layerCallback.store(true, std::memory_order_release);
        requestCallback();
    }
    void log(clap_log_severity severity, std::string_view text) const override {
        const std::string line(text);
        log(severity, line.c_str());
    }

    // The table index of a parameter the host and the editor may set: an
    // output is read-only. -1 otherwise.
    int editable(uint32_t id) const {
        const int i = table().indexOf(id);
        return i >= 0 && !table().at(static_cast<size_t>(i)).isOutput() ? i : -1;
    }

    // --- diagnostics ---------------------------------------------------------------

    // Main thread.
    void log(clap_log_severity severity, const char* text) const {
        if (hostLog && hostLog->log)
            hostLog->log(host, severity, text);
        else if (kDebugBuild)
            std::fprintf(stderr, "%s: %s\n", info().name.c_str(), text);
    }

    bool onMainThread() const {
        if (hostThreadCheck && hostThreadCheck->is_main_thread) return hostThreadCheck->is_main_thread(host);
        return std::this_thread::get_id() == mainThread;
    }
    // Without thread-check there is no telling: hosts may process on any
    // thread, the main one included.
    bool onAudioThread() const {
        return !(hostThreadCheck && hostThreadCheck->is_audio_thread) || hostThreadCheck->is_audio_thread(host);
    }

    // Debug builds: notes a call on a thread CLAP does not allow, for the
    // main thread to log. Any thread, and real-time safe.
    std::atomic<uint64_t> wrongThreads{0};  // every site seen called wrongly
    uint64_t loggedThreads = 0;             // main thread: those logged
    void expect(Thread thread, Site site) {
        if constexpr (!kDebugBuild) return;
        const bool ok = thread == Thread::Main    ? onMainThread()
                        : thread == Thread::Audio ? onAudioThread()
                        : active                  ? onAudioThread()
                                                  : onMainThread();
        if (ok) return;
        const uint64_t bit = uint64_t{1} << static_cast<unsigned>(site);
        // Nothing is asked of a host destroying the plugin: no callback could
        // reach it.
        if (!(wrongThreads.fetch_or(bit, std::memory_order_acq_rel) & bit) && site != Site::Destroy) requestCallback();
    }

    // What the audio thread found (Report bits), for the main thread to log.
    std::atomic<uint32_t> reports{0};
    uint32_t loggedReports = 0;   // main thread
    uint32_t safetyResetsSeen = 0;  // audio thread
    void report(uint32_t what) {
        if (!(reports.fetch_or(what, std::memory_order_acq_rel) & what)) requestCallback();
    }

    // Main thread: everything noted since the last time, once each.
    void logReports() {
        const uint32_t found = reports.load(std::memory_order_acquire) & ~loggedReports;
        loggedReports |= found;
        if (found & kNonFinite)
            log(CLAP_LOG_WARNING, "the processor produced a non-finite sample (NaN or infinity): the block was "
                                  "silenced and the processor reset");
        if (found & kNotesDropped)
            log(CLAP_LOG_WARNING, "more notes and MIDI in one block than the plugin can send: the rest were dropped");
        if (found & kBadBuses)
            log(CLAP_LOG_HOST_MISBEHAVING, "process() was called without the audio buses the plugin declared, or "
                                           "with one narrower than the selected layout");
        const uint64_t threads = wrongThreads.load(std::memory_order_acquire) & ~loggedThreads;
        loggedThreads |= threads;
        for (size_t k = 0; k < static_cast<size_t>(Site::kCount); ++k) {
            if (!(threads & (uint64_t{1} << k))) continue;
            char text[160];
            std::snprintf(text, sizeof(text), "%s was called on a thread CLAP does not allow for it", kSiteNames[k]);
            log(CLAP_LOG_HOST_MISBEHAVING, text);
        }
    }

    // --- whole edits, note names (main thread) ----------------------------------------

    // One complete edit of parameter i, as a menu item asks for: the host
    // hears a gesture around it, as from the editor.
    void editWhole(size_t i, double hostValue) {
        if (table().at(i).isOutput()) return;
        edits->begin(i);
        engine->store().set(i, hostValue);
        edits->value(i);
        edits->end(i);
        if (table().at(i).is(pflag::kRestart)) requestRestart();
        requestFlush();
        checkNoteNames();
    }

    NoteNames currentNoteNames() const {
        NoteNames names;
        if (namesNotes()) {
            ParamValues values(table());
            for (size_t i = 0; i < table().count(); ++i)
                values.setIndex(i, toPlain(table().at(i), engine->store().get(i)));
            noteNamesOf<Processor>(values, names);
        }
        return names;
    }
    // Tells the host when the names moved since it last read them.
    void checkNoteNames() {
        if (!namesNotes() || !hostNoteName || !hostNoteName->changed) return;
        NoteNames now = currentNoteNames();
        if (now == noteNameList) return;
        noteNameList = std::move(now);
        hostNoteName->changed(host);
    }

    // --- state (main thread) -------------------------------------------------------

    double currentEditorScale() const {
#if SUPPLE_WITH_GUI
        return editor ? editor->userScale() : editorScale;
#else
        return editorScale;
#endif
    }

    std::vector<uint8_t> save(state::Context context) {
        state::Snapshot s = snapshot();
        s.editorScale = currentEditorScale();
        if (presets && context == state::Context::Project) presets->describe(s);
        std::vector<uint8_t> bytes = state::save(magic, s, context);
        if (layer && layer->chunkTag() != 0)
            state::appendChunk(bytes, layer->chunkTag(), layer->saveChunk(s, context));
        return bytes;
    }

    bool load(const uint8_t* data, size_t size, state::Context context) {
        state::Snapshot snapshot(*definition);
        const state::LoadResult result = state::load(data, size, magic, snapshot);
        if (result == state::LoadResult::Corrupt) {
            log(CLAP_LOG_WARNING, "the state to load is damaged; nothing was loaded");
            return false;
        }
        if (result == state::LoadResult::NotOurs) {
            const bool legacy = Processor::loadLegacyState(data, size, [&](uint32_t id, double plainValue) {
                const int i = table().indexOf(id);
                if (i >= 0 && std::isfinite(plainValue))
                    snapshot.host[static_cast<size_t>(i)] = toHost(table().at(static_cast<size_t>(i)), plainValue);
            });
            if (!legacy) {
                log(CLAP_LOG_WARNING, "the state to load is not this plugin's; nothing was loaded");
                return false;
            }
        }
        // The layer's chunk may rewrite the snapshot, or refuse it, before
        // anything is applied.
        const uint8_t* chunk = nullptr;
        size_t chunkSize = 0;
        if (result == state::LoadResult::Ok && layer && layer->chunkTag() != 0 &&
            state::findChunk(data, size, layer->chunkTag(), chunk, chunkSize) &&
            !layer->loadChunk(chunk, chunkSize, snapshot, context)) {
            log(CLAP_LOG_WARNING, "the state to load has a chunk this plugin cannot read; nothing was loaded");
            return false;
        }
        apply(snapshot, context);
        return true;
    }

    // Everything the snapshot holds, for its context: a preset changes the
    // sound and leaves the session (editor size, perProject() properties,
    // which preset is loaded) as it was.
    void apply(const state::Snapshot& snapshot, state::Context context) override {
        const bool session = context == state::Context::Project;
        applySound(snapshot, session);
        if (session && snapshot.editorScale > 0.0) restoreEditorScale(snapshot.editorScale);
        if (session && presets) presets->restore(snapshot);
        afterApply();
    }
    void applyValues(const state::Snapshot& snapshot) override {
        applySound(snapshot, true);
        afterApply();
    }

    // The values and properties; perProject() properties only with `perProject`.
    void applySound(const state::Snapshot& snapshot, bool perProject) {
        applyRestart = active && restartParamsDiffer(snapshot.host);
        // Only the stores are written here; the audio thread applies them.
        // Outputs are the processor's, and state does not carry them.
        for (size_t i = 0; i < table().count(); ++i)
            if (!table().at(i).isOutput()) engine->store().set(i, snapshot.host[i]);
        // A file property's file is relocated if it moved, and loaded before
        // this returns, so the next block plays it.
        for (size_t i = 0; i < propertyList().count(); ++i) {
            if (!perProject && propertyList().at(i).perProjectOnly()) continue;
            engine->setProperty(i, snapshot.hasProperty[i] ? std::string_view(snapshot.property[i])
                                                           : std::string_view(propertyList().at(i).def),
                                true);
        }
    }

    // Hosts hear that the values moved.
    void afterApply() {
        if (hostParams && hostParams->rescan) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        if (applyRestart) requestRestart();
        applyRestart = false;
        // A stopped host would otherwise apply the new values only when it
        // next processes; a flush applies them now.
        if (active) requestFlush();
        checkNoteNames();
    }

    void restoreEditorScale(double scale) {
        editorScale = scale;
#if SUPPLE_WITH_GUI
        if (editor) {
            // The host decides: ask first, and resize only if it agrees. A
            // refusal keeps the open editor as it is, and that is what saves.
            uint32_t w = 0, h = 0, cw = 0, ch = 0;
            editor->sizeAt(scale, w, h);
            editor->size(cw, ch);
            if (w != cw || h != ch) {
                if (hostGui && hostGui->request_resize && hostGui->request_resize(host, w, h))
                    editor->setUserScale(scale);
                else
                    editorScale = editor->userScale();
            }
        }
#endif
    }

    // The session changed from inside the plugin; the host should save it.
    void markDirty() override {
        if (hostState && hostState->mark_dirty) hostState->mark_dirty(host);
    }

    // --- StateAccess: the preset library's loads are the plugin's own -------------
    std::vector<uint8_t> saveState(state::Context context) override { return save(context); }
    bool loadState(const uint8_t* data, size_t size, state::Context context) override {
        if (!load(data, size, context)) return false;
        markDirty();
        return true;
    }

    void createPresets() {
        if (!info().presets) return;
        presets = std::make_unique<PresetLibrary>(*this, PresetLibrary::defaultUserFolder(info()), definition);
        presets->onLoaded = [this](const PresetLibrary::Entry& entry) {
            if (!hostPresetLoad || !hostPresetLoad->loaded) return;
            if (entry.user()) {
                const std::string path = PresetLibrary::toUtf8(entry.file);
                hostPresetLoad->loaded(host, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), nullptr);
            } else {
                hostPresetLoad->loaded(host, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, entry.name.c_str());
            }
        };
    }

    // Reasons for the next on_main_thread, set from any thread.
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> restartWanted{false};
    std::atomic<bool> flushWanted{false};
    std::atomic<bool> flushRequested{false};
    std::atomic<bool> voicesChanged{false};
    std::atomic<bool> namesChanged{false};

    template <class T>
    T* hostExtension(const char* id) const {
        return host && host->get_extension ? static_cast<T*>(const_cast<void*>(host->get_extension(host, id)))
                                           : nullptr;
    }

    // Any thread.
    void requestCallback() {
        if (!callbackRequested.exchange(true, std::memory_order_acq_rel) && host && host->request_callback)
            host->request_callback(host);
    }

    // Main thread.
    void requestRestart() {
        if (!active || restartOutstanding || !host || !host->request_restart) return;
        restartOutstanding = true;
        host->request_restart(host);
    }

    // Main thread. Asks the host to call process() or flush(), which is where
    // edits reach it -- with the transport stopped, nothing else would.
    void requestFlush() {
        if (flushRequested.exchange(true, std::memory_order_acq_rel)) return;
        if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
    }

    // Audio thread, or main thread while inactive.
    void drainEdits(const clap_output_events_t* out) {
        flushRequested.store(false, std::memory_order_release);
        if (!out) return;
        const bool delivered = edits->drain([&](EditBridge::Kind kind, size_t i) {
            const uint32_t id = table().at(i).id;
            if (kind == EditBridge::Kind::Value) {
                clap_event_param_value_t ev{};
                ev.header.size = sizeof(ev);
                ev.header.time = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type = CLAP_EVENT_PARAM_VALUE;
                ev.param_id = id;
                ev.note_id = -1;
                ev.port_index = -1;
                ev.channel = -1;
                ev.key = -1;
                ev.value = engine->store().get(i);
                return out->try_push(out, &ev.header);
            }
            clap_event_param_gesture_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = static_cast<uint16_t>(kind == EditBridge::Kind::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                                  : CLAP_EVENT_PARAM_GESTURE_END);
            ev.param_id = id;
            return out->try_push(out, &ev.header);
        });
        if (!delivered) {
            // request_flush is forbidden on the audio thread; ask from main.
            flushWanted.store(true, std::memory_order_release);
            requestCallback();
        }
    }

    // After the engine ran (audio thread, or main thread while inactive).
    // `frames` is the block length, 0 for a flush.
    void afterAudioWork(const clap_output_events_t* out, uint32_t frames) {
        if (engine->properties().needsCollection()) requestCallback();
        if (engine->takeRestartRequest()) {
            restartWanted.store(true, std::memory_order_release);
            requestCallback();
        }
        // Only ever set while active, so this is the audio thread, as the
        // tail extension requires.
        if (engine->takeTailChange() && hostTail && hostTail->changed) hostTail->changed(host);
        if (engine->takeVoiceChange()) {  // voice_info.changed is main-thread only
            voicesChanged.store(true, std::memory_order_release);
            requestCallback();
        }
        if (engine->takeNoteNamesChange() && namesNotes()) {  // so is note_name.changed
            namesChanged.store(true, std::memory_order_release);
            requestCallback();
        }
        if (const uint32_t resets = engine->safetyResets(); resets != safetyResetsSeen) {
            safetyResetsSeen = resets;
            report(kNonFinite);
        }
        const NoteOutput& sent = engine->noteOutput();
        if (sent.dropped() > 0) report(kNotesDropped);
        int delivered = 0;
        for (; out && delivered < sent.count(); ++delivered) {
            const NoteOutput::Entry& entry = sent[delivered];
            const uint32_t time = frames ? std::min(entry.time, frames - 1) : 0;
            bool pushed = false;
            if (entry.kind == NoteOutput::Kind::Midi) {
                clap_event_midi_t e{};
                e.header.size = sizeof(e);
                e.header.time = time;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.type = CLAP_EVENT_MIDI;
                e.port_index = 0;
                std::memcpy(e.data, entry.midi.data, 3);
                pushed = out->try_push(out, &e.header);
            } else {
                // A note end names the note as it arrived, on the input port;
                // the processor's own notes leave on the output port.
                const NoteEvent& n = entry.note;
                const bool end = entry.kind == NoteOutput::Kind::End;
                clap_event_note_t e{};
                e.header.size = sizeof(e);
                e.header.time = time;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.type = static_cast<uint16_t>(end                               ? CLAP_EVENT_NOTE_END
                                                      : n.type == NoteEvent::Type::On   ? CLAP_EVENT_NOTE_ON
                                                      : n.type == NoteEvent::Type::Off  ? CLAP_EVENT_NOTE_OFF
                                                                                        : CLAP_EVENT_NOTE_CHOKE);
                e.note_id = n.noteId;
                e.port_index = end ? n.port : 0;
                e.channel = n.channel;
                e.key = n.key;
                e.velocity = end ? 0.0 : std::clamp(n.velocity, 0.0, 1.0);
                pushed = out->try_push(out, &e.header);
            }
            if (!pushed) break;
        }
        engine->deferNotes(delivered);
        // Outputs last: stamped at the block's final frame, after every note
        // end, since the host's list must stay in time order.
        if (engine->takeOutputsDue()) publishOutputs(out, frames);
    }

    // Audio thread. Each output whose value moved since the host last heard
    // it; one the queue refuses is offered again next time.
    void publishOutputs(const clap_output_events_t* out, uint32_t frames) {
        if (!out) return;
        for (size_t k = 0; k < table().outputs().size(); ++k) {
            const size_t i = table().outputs()[k];
            const double v = engine->store().get(i);
            if (!resendOutputs && v == heardOutputs[k].load(std::memory_order_relaxed)) continue;
            clap_event_param_value_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = frames ? frames - 1 : 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.param_id = table().at(i).id;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = v;
            if (out->try_push(out, &ev.header)) heardOutputs[k].store(v, std::memory_order_relaxed);
            else return;  // the rest next time, the resend included
        }
        resendOutputs = false;
    }

    // Main thread: parameters whose change requires a restart, compared
    // against the values the active processor was built with.
    bool restartParamsDiffer(const std::vector<double>& hostValues) const {
        if (!table().anyRestart()) return false;
        for (size_t i = 0; i < table().count(); ++i)
            if (table().at(i).is(pflag::kRestart) && hostValues[i] != engine->store().get(i)) return true;
        return false;
    }

    // --- redefinition (main thread) ----------------------------------------------------

    bool redefine(DefinitionPtr next) override {
        if (!next || next->info().id != info().id || next->info().stateTag != info().stateTag ||
            !next->problems().empty())
            return false;
        pendingDefinition = std::move(next);
        if (!active) swapDefinition();
        else requestRestart();  // the swap happens as the host deactivates
        return true;
    }

    // While inactive: the pending definition replaces the current one. What
    // the instance holds carries over through the state, by parameter ID and
    // the layer's chunk.
    void swapDefinition() {
        DefinitionPtr next = std::move(pendingDefinition);
        pendingDefinition.reset();
        if (!next || next == definition) return;
        const std::vector<uint8_t> carried = save(state::Context::Project);
        const PluginInfo before = info();
        const int channelsBefore = channels;
        const bool offline = engine->offline();
        definition = std::move(next);
        engine = std::make_unique<Engine>(definition);
        engine->setOffline(offline);
        if (info().tuning) engine->setTuning(&tuning);
        edits = std::make_unique<EditBridge>(table().count());
        heardOutputs = std::make_unique<std::atomic<double>[]>(std::max<size_t>(1, table().outputs().size()));
        for (size_t k = 0; k < table().outputs().size(); ++k)
            heardOutputs[k].store(hostDefault(table().at(table().outputs()[k])), std::memory_order_relaxed);
        resendOutputs = true;
        if (!info().offers(channels)) channels = info().layouts.empty() ? 0 : info().layouts[0].channels;
        presets.reset();
        createPresets();
        load(carried.data(), carried.size(), state::Context::Project);
        if (hostParams && hostParams->rescan) hostParams->rescan(host, CLAP_PARAM_RESCAN_ALL);
        const PluginInfo& after = info();
        const bool ports = before.kind != after.kind || before.sidechain != after.sidechain ||
                           before.auxOutputs != after.auxOutputs || before.layouts != after.layouts ||
                           channels != channelsBefore;
        if (ports && hostAudioPorts && hostAudioPorts->rescan) {
            const uint32_t flags = CLAP_AUDIO_PORTS_RESCAN_NAMES | CLAP_AUDIO_PORTS_RESCAN_FLAGS |
                                   CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT | CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE |
                                   CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR | CLAP_AUDIO_PORTS_RESCAN_LIST;
            hostAudioPorts->rescan(host, flags);
        }
        if ((before.takesNotes() != after.takesNotes() || before.sendsNotes() != after.sendsNotes()) && hostNotePorts &&
            hostNotePorts->rescan)
            hostNotePorts->rescan(host, CLAP_NOTE_PORTS_RESCAN_ALL);
        if (namesNotes() && !hostNoteName) hostNoteName = hostExtension<const clap_host_note_name_t>(CLAP_EXT_NOTE_NAME);
        checkNoteNames();
#if SUPPLE_WITH_GUI
        if (editor) {
            // The editor shows the old table: a new one for the new, at the
            // same scale if the host agrees to the size.
            editor->rebuild(*this);
            engine->setMetering(true);
            uint32_t w = 0, h = 0, cw = 0, ch = 0;
            editor->sizeAt(editor->userScale(), w, h);
            editor->size(cw, ch);
            if ((w != cw || h != ch) && hostGui && hostGui->request_resize) hostGui->request_resize(host, w, h);
        }
#endif
        if (layer) layer->redefined();
    }

#if SUPPLE_WITH_GUI
    std::unique_ptr<gui::Gui> editor;
    clap_id timerId = CLAP_INVALID_ID;
    double platformScale = 1.0;

    // --- gui::EditorHost (main thread) ---------------------------------------
    const Definition& pluginDefinition() const override { return *definition; }
    double value(uint32_t id) const override {
        const int i = table().indexOf(id);
        return i >= 0 ? engine->store().get(static_cast<size_t>(i)) : 0.0;
    }
    double modulation(uint32_t id) const override {
        const int i = table().indexOf(id);
        return i >= 0 ? engine->modulation(static_cast<size_t>(i)) : 0.0;
    }

    void beginEdit(uint32_t id) override {
        const int i = editable(id);
        if (i < 0) return;
        edits->begin(static_cast<size_t>(i));
        requestFlush();
    }
    void setValue(uint32_t id, double v) override {
        const int i = editable(id);
        if (i < 0) return;
        const size_t index = static_cast<size_t>(i);
        engine->store().set(index, v);
        edits->value(index);
        if (table().at(index).is(pflag::kRestart)) requestRestart();
        requestFlush();
    }
    void endEdit(uint32_t id) override {
        const int i = editable(id);
        if (i < 0) return;
        edits->end(static_cast<size_t>(i));
        requestFlush();
        checkNoteNames();
    }
    std::string_view property(uint32_t id) const override {
        const int i = propertyList().indexOf(id);
        return i >= 0 ? std::string_view(engine->properties().get(static_cast<size_t>(i))) : std::string_view();
    }
    bool setProperty(uint32_t id, std::string_view value) override {
        const int i = propertyList().indexOf(id);
        if (i < 0) return false;
        const uint64_t before = engine->properties().version();
        if (!engine->setProperty(static_cast<size_t>(i), value)) return false;
        // The user chose this file: its folder helps find others.
        if (propertyList().at(static_cast<size_t>(i)).isFile() && !value.empty())
            engine->fileChosen(static_cast<size_t>(i));
        if (engine->properties().version() != before) {
            markDirty();
            if (active) requestFlush();
        }
        return true;
    }
    uint64_t propertiesVersion() const override { return engine->properties().version(); }
    FileStatus fileStatus(uint32_t id) const override {
        const int i = propertyList().indexOf(id);
        return i >= 0 ? engine->fileStatus(static_cast<size_t>(i)) : FileStatus{};
    }
    PresetLibrary* presetLibrary() override { return presets.get(); }

    gui::TrackInfo trackInfo;
    uint64_t trackChanges = 0;
    gui::TrackInfo track() const override { return trackInfo; }
    uint64_t trackVersion() const override { return trackChanges; }
    // Asks the host again, at start-up and whenever it says the track changed.
    void refreshTrack() {
        clap_track_info_t t{};
        if (!hostTrackInfo || !hostTrackInfo->get || !hostTrackInfo->get(host, &t)) return;
        gui::TrackInfo next;
        if (t.flags & CLAP_TRACK_INFO_HAS_TRACK_NAME) next.name.assign(t.name, strnlen(t.name, sizeof(t.name)));
        if (t.flags & CLAP_TRACK_INFO_HAS_TRACK_COLOR) {
            next.hasColour = true;
            next.red = t.color.red;
            next.green = t.color.green;
            next.blue = t.color.blue;
            next.alpha = t.color.alpha;
        }
        next.returnTrack = (t.flags & CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK) != 0;
        next.bus = (t.flags & CLAP_TRACK_INFO_IS_FOR_BUS) != 0;
        next.master = (t.flags & CLAP_TRACK_INFO_IS_FOR_MASTER) != 0;
        trackInfo = std::move(next);
        ++trackChanges;
    }

    bool hostPopup(uint32_t param, float x, float y) override {
        if (!editor || !hostContextMenu || !hostContextMenu->can_popup || !hostContextMenu->popup ||
            !hostContextMenu->can_popup(host))
            return false;
        int32_t hx = 0, hy = 0;
        editor->window().designToHost(x, y, hx, hy);
        const clap_context_menu_target_t target{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, param};
        return hostContextMenu->popup(host, &target, 0, hx, hy);
    }
    bool hostMenu(uint32_t param, gui::HostMenu& menu) override {
        if (!hostContextMenu || !hostContextMenu->populate) return false;
        // Every kind is accepted and flattened: a submenu's label becomes a
        // header over its items, and its end a separator.
        clap_context_menu_builder_t builder{};
        builder.ctx = &menu;
        builder.supports = [](const clap_context_menu_builder_t*, clap_context_menu_item_kind_t kind) {
            return kind <= CLAP_CONTEXT_MENU_ITEM_TITLE;
        };
        builder.add_item = [](const clap_context_menu_builder_t* b, clap_context_menu_item_kind_t kind,
                              const void* data) {
            auto& m = *static_cast<gui::HostMenu*>(b->ctx);
            auto text = [](const char* t) { return std::string(t ? t : ""); };
            switch (kind) {
                case CLAP_CONTEXT_MENU_ITEM_ENTRY: {
                    const auto* e = static_cast<const clap_context_menu_entry_t*>(data);
                    if (!e) return false;
                    m.items.push_back(gui::MenuItem::item(text(e->label), false, e->is_enabled));
                    m.actions.push_back(e->action_id);
                    return true;
                }
                case CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY: {
                    const auto* e = static_cast<const clap_context_menu_check_entry_t*>(data);
                    if (!e) return false;
                    m.items.push_back(gui::MenuItem::item(text(e->label), e->is_checked, e->is_enabled));
                    m.actions.push_back(e->action_id);
                    return true;
                }
                case CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU: {
                    const auto* e = static_cast<const clap_context_menu_submenu_t*>(data);
                    m.items.push_back(gui::MenuItem::header(text(e ? e->label : nullptr)));
                    break;
                }
                case CLAP_CONTEXT_MENU_ITEM_TITLE: {
                    const auto* e = static_cast<const clap_context_menu_item_title_t*>(data);
                    m.items.push_back(gui::MenuItem::header(text(e ? e->title : nullptr)));
                    break;
                }
                case CLAP_CONTEXT_MENU_ITEM_SEPARATOR:
                case CLAP_CONTEXT_MENU_ITEM_END_SUBMENU: m.items.push_back(gui::MenuItem::separator()); break;
                default: return false;
            }
            m.actions.push_back(CLAP_INVALID_ID);
            return true;
        };
        const clap_context_menu_target_t target{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, param};
        return hostContextMenu->populate(host, &target, &builder);
    }
    bool performHostMenu(uint32_t param, uint32_t action) override {
        if (!hostContextMenu || !hostContextMenu->perform || action == CLAP_INVALID_ID) return false;
        const clap_context_menu_target_t target{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, param};
        return hostContextMenu->perform(host, &target, action);
    }

    void log(gui::LogLevel level, std::string_view message) override {
        const clap_log_severity severity = level == gui::LogLevel::Debug     ? CLAP_LOG_DEBUG
                                           : level == gui::LogLevel::Info    ? CLAP_LOG_INFO
                                           : level == gui::LogLevel::Warning ? CLAP_LOG_WARNING
                                                                             : CLAP_LOG_ERROR;
        const std::string text(message);
        log(severity, text.c_str());
    }

    void playNote(int key, double velocity, bool on) override {
        engine->playNote(key, velocity, on);  // played at the start of the next block
        if (info().takesNotes() && host && host->request_process) host->request_process(host);
    }
    size_t readAudio(FeedFrame* out, size_t max) override {
        AudioFeed* feed = engine->feed();
        return feed ? feed->read(out, max) : 0;
    }
    bool popTelemetry(TelemetryFrame& frame) override { return engine->telemetry().pop(frame); }
    int latencySamples() const override { return engine->latency(); }
    double sampleRate() const override { return engine->sampleRate(); }

    void startTimer() {
        if (timerId != CLAP_INVALID_ID || !hostTimer || !hostTimer->register_timer) return;
        clap_id id = CLAP_INVALID_ID;
        if (hostTimer->register_timer(host, kTimerMs, &id)) timerId = id;
    }
    void stopTimer() {
        if (timerId != CLAP_INVALID_ID && hostTimer && hostTimer->unregister_timer)
            hostTimer->unregister_timer(host, timerId);
        timerId = CLAP_INVALID_ID;
    }
    void closeEditor() {
        engine->setMetering(false);
        stopTimer();
        if (editor) editorScale = editor->userScale();
        editor.reset();
    }
#endif
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// --- audio ports ----------------------------------------------------------------------

// Inputs: the main bus (an effect), then the sidechain. Outputs: the main bus
// (all but a note effect), then each aux output. Every port is as wide as the
// selected layout. IDs are stable: main 0, sidechain 1, aux k is 1 + k.
uint32_t inputPorts(const PluginInfo& info) {
    return (info.audioInput() ? 1u : 0u) + (info.sidechain ? 1u : 0u);
}
uint32_t outputPorts(const PluginInfo& info) {
    return info.audioOutput() ? 1u + static_cast<uint32_t>(info.auxCount()) : 0u;
}
uint32_t sidechainPort(const PluginInfo& info) { return info.audioInput() ? 1u : 0u; }  // its input index

// The declared layout this wide, or Generic.
Layout layoutOf(const PluginInfo& info, int channels) {
    for (const BusLayout& l : info.layouts)
        if (l.channels == channels) return l.layout;
    return Layout::Generic;
}
bool anyLayout(const PluginInfo& info, bool (*test)(Layout)) {
    for (const BusLayout& l : info.layouts)
        if (test(l.layout)) return true;
    return false;
}

const char* portType(const PluginInfo& info, int channels) {
    const Layout l = layoutOf(info, channels);
    if (l == Layout::Mono) return CLAP_PORT_MONO;
    if (l == Layout::Stereo) return CLAP_PORT_STEREO;
    if (isSurround(l)) return CLAP_PORT_SURROUND;
    if (isAmbisonic(l)) return CLAP_PORT_AMBISONIC;
    return nullptr;
}

bool portExists(const PluginInfo& info, bool isInput, uint32_t index) {
    return index < (isInput ? inputPorts(info) : outputPorts(info));
}

uint32_t CLAP_ABI portsCount(const clap_plugin_t* p, bool isInput) {
    return isInput ? inputPorts(self(p)->info()) : outputPorts(self(p)->info());
}

bool CLAP_ABI portsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* out) {
    const PluginInfo& info = self(p)->info();
    if (!portExists(info, isInput, index)) return false;
    const int channels = self(p)->channels;
    const bool main = isInput ? info.audioInput() && index == 0 : index == 0;
    std::memset(out, 0, sizeof(*out));
    if (isInput) {
        out->id = main ? 0 : 1;
        std::snprintf(out->name, sizeof(out->name), "%s", main ? "Input" : "Sidechain");
    } else {
        out->id = index;
        std::snprintf(out->name, sizeof(out->name), "%s", main ? "Output" : info.auxOutputs[index - 1].c_str());
    }
    out->flags = CLAP_AUDIO_PORT_SUPPORTS_64BITS | CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
    if (main) out->flags |= CLAP_AUDIO_PORT_IS_MAIN;
    out->channel_count = static_cast<uint32_t>(channels);
    out->port_type = portType(info, channels);
    // In-place pairing is reciprocal, and only between the main buses.
    out->in_place_pair = main && info.audioInput() ? 0 : CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {portsCount, portsGet};

uint32_t CLAP_ABI configsCount(const clap_plugin_t* p) {
    return static_cast<uint32_t>(self(p)->info().layouts.size());
}

// A configuration's ID is its channel count: stable, and meaningful in logs.
bool CLAP_ABI configsGet(const clap_plugin_t* p, uint32_t index, clap_audio_ports_config_t* out) {
    const PluginInfo& info = self(p)->info();
    if (index >= info.layouts.size()) return false;
    const int channels = info.layouts[index].channels;
    std::memset(out, 0, sizeof(*out));
    out->id = static_cast<clap_id>(channels);
    const char* names[] = {"", "Mono", "Stereo", "5.1", "7.1", "7.1.4", "Ambisonic 1st order", "Ambisonic 2nd order",
                           "Ambisonic 3rd order"};
    const Layout l = layoutOf(info, channels);
    if (l == Layout::Generic)
        std::snprintf(out->name, sizeof(out->name), "%d channels", channels);
    else
        std::snprintf(out->name, sizeof(out->name), "%s", names[static_cast<int>(l)]);
    out->input_port_count = inputPorts(info);
    out->output_port_count = outputPorts(info);
    out->has_main_input = info.audioInput();
    out->main_input_channel_count = info.audioInput() ? static_cast<uint32_t>(channels) : 0;
    out->main_input_port_type = info.audioInput() ? portType(info, channels) : nullptr;
    out->has_main_output = true;
    out->main_output_channel_count = static_cast<uint32_t>(channels);
    out->main_output_port_type = portType(info, channels);
    return true;
}

bool CLAP_ABI configsSelect(const clap_plugin_t* p, clap_id id) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::PortsConfig);
    if (s->active) return false;
    for (const BusLayout& l : s->info().layouts) {
        if (static_cast<clap_id>(l.channels) == id) {
            s->channels = l.channels;
            return true;
        }
    }
    return false;
}

const clap_plugin_audio_ports_config_t kAudioPortsConfig = {configsCount, configsGet, configsSelect};

// A host asking for widths port by port (clap-wrapper does, for a VST3
// host's speaker arrangements): every port takes the main bus's width, so
// the requests must agree on one declared layout. Ports a request leaves out
// follow it.
bool layoutRequested(const PluginInfo& info, const clap_audio_port_configuration_request_t* requests, uint32_t count,
                     int& channels) {
    if (!requests || count == 0) return false;
    int width = -1;
    for (uint32_t k = 0; k < count; ++k) {
        const auto& r = requests[k];
        if (!portExists(info, r.is_input, r.port_index)) return false;
        const int w = static_cast<int>(r.channel_count);
        if (width >= 0 && w != width) return false;
        width = w;
    }
    if (!info.offers(width)) return false;
    const Layout l = layoutOf(info, width);
    for (uint32_t k = 0; k < count; ++k) {
        const auto& r = requests[k];
        const char* type = portType(info, width);
        if (r.port_type && (!type || std::strcmp(r.port_type, type) != 0)) return false;
        if (!r.port_type || !r.port_details) continue;
        if (isSurround(l) && std::memcmp(r.port_details, speakers(l), static_cast<size_t>(width)) != 0) return false;
        if (isAmbisonic(l)) {
            const auto* a = static_cast<const clap_ambisonic_config_t*>(r.port_details);
            if (a->ordering != CLAP_AMBISONIC_ORDERING_ACN || a->normalization != CLAP_AMBISONIC_NORMALIZATION_SN3D)
                return false;
        }
    }
    channels = width;
    return true;
}

bool CLAP_ABI configurableCanApply(const clap_plugin_t* p, const clap_audio_port_configuration_request_t* requests,
                                   uint32_t count) {
    int channels = 0;
    return !self(p)->active && layoutRequested(self(p)->info(), requests, count, channels);
}

bool CLAP_ABI configurableApply(const clap_plugin_t* p, const clap_audio_port_configuration_request_t* requests,
                                uint32_t count) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::PortsConfig);
    int channels = 0;
    if (s->active || !layoutRequested(s->info(), requests, count, channels)) return false;
    s->channels = channels;
    return true;
}

const clap_plugin_configurable_audio_ports_t kConfigurablePorts = {configurableCanApply, configurableApply};

// Surround and ambisonic layouts say which channel is which.
bool CLAP_ABI surroundMaskSupported(const clap_plugin_t* p, uint64_t mask) {
    for (const BusLayout& l : self(p)->info().layouts)
        if (isSurround(l.layout) && speakerMask(l.layout, l.channels) == mask) return true;
    return false;
}

uint32_t CLAP_ABI surroundChannelMap(const clap_plugin_t* p, bool isInput, uint32_t port, uint8_t* map,
                                     uint32_t capacity) {
    const int channels = self(p)->channels;
    const Layout l = layoutOf(self(p)->info(), channels);
    if (!portExists(self(p)->info(), isInput, port) || !isSurround(l) || !map || capacity < static_cast<uint32_t>(channels))
        return 0;
    std::memcpy(map, speakers(l), static_cast<size_t>(channels));
    return static_cast<uint32_t>(channels);
}

const clap_plugin_surround_t kSurround = {surroundMaskSupported, surroundChannelMap};

bool CLAP_ABI ambisonicSupported(const clap_plugin_t*, const clap_ambisonic_config_t* config) {
    return config && config->ordering == CLAP_AMBISONIC_ORDERING_ACN &&
           config->normalization == CLAP_AMBISONIC_NORMALIZATION_SN3D;
}

bool CLAP_ABI ambisonicConfig(const clap_plugin_t* p, bool isInput, uint32_t port, clap_ambisonic_config_t* config) {
    if (!portExists(self(p)->info(), isInput, port) || !config || !isAmbisonic(layoutOf(self(p)->info(), self(p)->channels)))
        return false;
    config->ordering = CLAP_AMBISONIC_ORDERING_ACN;
    config->normalization = CLAP_AMBISONIC_NORMALIZATION_SN3D;
    return true;
}

const clap_plugin_ambisonic_t kAmbisonic = {ambisonicSupported, ambisonicConfig};

// --- notes --------------------------------------------------------------------------------

uint32_t CLAP_ABI notePortsCount(const clap_plugin_t* p, bool isInput) {
    const PluginInfo& info = self(p)->info();
    return (isInput ? info.takesNotes() : info.sendsNotes()) ? 1 : 0;
}

bool CLAP_ABI notePortsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_note_port_info_t* out) {
    const PluginInfo& info = self(p)->info();
    if (index != 0 || !(isInput ? info.takesNotes() : info.sendsNotes())) return false;
    std::memset(out, 0, sizeof(*out));
    out->id = 0;
    out->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    out->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(out->name, sizeof(out->name), "%s", isInput ? "Notes" : "Notes Out");
    return true;
}

const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

// A plugin with a voice pool tells the host about its voices.
bool voiceInfo(const Plugin& s) { return s.info().takesNotes() && s.voiceCapacity() > 0; }

bool CLAP_ABI voiceInfoGet(const clap_plugin_t* p, clap_voice_info_t* out) {
    if (!out) return false;
    const int capacity = self(p)->voiceCapacity();
    out->voice_capacity = static_cast<uint32_t>(capacity);
    out->voice_count = static_cast<uint32_t>(std::clamp(self(p)->engine->voices(), 0, capacity));
    out->flags = Processor::kOverlappingNotes || self(p)->info().overlappingNotes
                     ? CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES
                     : 0;
    return true;
}

const clap_plugin_voice_info_t kVoiceInfoExt = {voiceInfoGet};

// clap-wrapper's VST3 extension: which note expressions a VST3 host may
// offer (product.cmake's PRODUCT_NOTE_EXPRESSIONS), and every MIDI channel.
uint32_t CLAP_ABI vst3MidiChannels(const clap_plugin_t*, uint32_t) { return 16; }
uint32_t CLAP_ABI vst3NoteExpressions(const clap_plugin_t* p) { return self(p)->info().noteExpressions; }
const clap_plugin_as_vst3_t kAsVst3 = {vst3MidiChannels, vst3NoteExpressions};

// --- latency and tail ---------------------------------------------------------------------

// The value from the last activation: latency may only change across one.
uint32_t CLAP_ABI latencyGet(const clap_plugin_t* p) {
    self(p)->expect(Thread::Main, Site::Latency);
    return static_cast<uint32_t>(self(p)->engine->latency());
}
const clap_plugin_latency_t kLatency = {latencyGet};

uint32_t CLAP_ABI tailGet(const clap_plugin_t* p) { return self(p)->engine->tail(); }
const clap_plugin_tail_t kTail = {tailGet};

// --- params ----------------------------------------------------------------------------------

uint32_t CLAP_ABI paramsCount(const clap_plugin_t* p) {
    self(p)->expect(Thread::Main, Site::ParamsCount);
    return static_cast<uint32_t>(self(p)->table().count());
}

bool CLAP_ABI paramsInfo(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* out) {
    self(plugin)->expect(Thread::Main, Site::ParamsInfo);
    if (index >= self(plugin)->table().count()) return false;
    const ParamDef& p = self(plugin)->table().at(index);
    std::memset(out, 0, sizeof(*out));
    out->id = p.id;
    out->flags = 0;
    if (p.is(pflag::kAutomatable)) out->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    if (p.is(pflag::kModulatable)) out->flags |= CLAP_PARAM_IS_MODULATABLE;
    if (p.is(pflag::kPolyMod))
        out->flags |= CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
                      CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_PORT;
    if (p.is(pflag::kHidden)) out->flags |= CLAP_PARAM_IS_HIDDEN;
    if (p.is(pflag::kBypass)) out->flags |= CLAP_PARAM_IS_BYPASS;
    if (p.isOutput()) out->flags |= CLAP_PARAM_IS_READONLY;
    if (p.stepped()) out->flags |= CLAP_PARAM_IS_STEPPED;
    if (p.kind == ParamKind::Choice) out->flags |= CLAP_PARAM_IS_ENUM;
    std::snprintf(out->name, sizeof(out->name), "%s", p.name);
    std::snprintf(out->module, sizeof(out->module), "%s", p.group);
    out->min_value = p.hostMin();
    out->max_value = p.hostMax();
    out->default_value = hostDefault(p);
    return true;
}

bool CLAP_ABI paramsValue(const clap_plugin_t* p, clap_id id, double* out) {
    self(p)->expect(Thread::Main, Site::ParamsValue);
    const ParamTable& table = self(p)->table();
    const int i = table.indexOf(id);
    if (i < 0) return false;
    if (table.at(static_cast<size_t>(i)).isOutput()) {
        for (size_t k = 0; k < table.outputs().size(); ++k)
            if (table.outputs()[k] == i) *out = self(p)->heardOutputs[k].load(std::memory_order_relaxed);
        return true;
    }
    *out = self(p)->engine->store().get(static_cast<size_t>(i));
    return true;
}

bool CLAP_ABI paramsValueToText(const clap_plugin_t* plugin, clap_id id, double value, char* out, uint32_t size) {
    self(plugin)->expect(Thread::Main, Site::ParamsToText);
    const ParamDef* p = self(plugin)->table().find(id);
    if (!p || !out || size == 0) return false;
    formatValue(*p, value, out, size);
    return true;
}

bool CLAP_ABI paramsTextToValue(const clap_plugin_t* plugin, clap_id id, const char* text, double* out) {
    self(plugin)->expect(Thread::Main, Site::ParamsFromText);
    const ParamDef* p = self(plugin)->table().find(id);
    return p && out && parseValue(*p, text, *out);
}

void CLAP_ABI paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in,
                          const clap_output_events_t* out) SUPPLE_REALTIME {
    Plugin* s = self(p);
    s->expect(Thread::AudioWhileActive, Site::ParamsFlush);
    ClapEvents events(s->table(), s->info().takesNotes(), in, s->tuningSpace);
    s->engine->flush(events);
    s->drainEdits(out);
    s->afterAudioWork(out, 0);
}

const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue,
                                      paramsFlush};

// --- state ---------------------------------------------------------------------------------

bool writeAll(const clap_ostream_t* stream, const std::vector<uint8_t>& bytes) {
    size_t written = 0;
    while (written < bytes.size()) {
        const int64_t n = stream->write(stream, bytes.data() + written, bytes.size() - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

bool readAll(const clap_istream_t* stream, std::vector<uint8_t>& bytes) {
    uint8_t chunk[4096];
    for (;;) {
        const int64_t n = stream->read(stream, chunk, sizeof(chunk));
        if (n < 0) return false;
        if (n == 0) return true;
        bytes.insert(bytes.end(), chunk, chunk + n);
        if (bytes.size() > state::kMaxBytes) return false;  // not ours, whatever it is
    }
}

bool CLAP_ABI stateSave(const clap_plugin_t* p, const clap_ostream_t* stream) {
    self(p)->expect(Thread::Main, Site::StateSave);
    return writeAll(stream, self(p)->save(state::Context::Project));
}

bool CLAP_ABI stateLoad(const clap_plugin_t* p, const clap_istream_t* stream) {
    self(p)->expect(Thread::Main, Site::StateLoad);
    std::vector<uint8_t> bytes;
    return readAll(stream, bytes) && self(p)->load(bytes.data(), bytes.size(), state::Context::Project);
}

state::Context contextOf(uint32_t type) {
    return type == CLAP_STATE_CONTEXT_FOR_PRESET ? state::Context::Preset : state::Context::Project;
}

bool CLAP_ABI stateContextSave(const clap_plugin_t* p, const clap_ostream_t* stream, uint32_t type) {
    self(p)->expect(Thread::Main, Site::StateSave);
    return writeAll(stream, self(p)->save(contextOf(type)));
}

bool CLAP_ABI stateContextLoad(const clap_plugin_t* p, const clap_istream_t* stream, uint32_t type) {
    self(p)->expect(Thread::Main, Site::StateLoad);
    std::vector<uint8_t> bytes;
    return readAll(stream, bytes) && self(p)->load(bytes.data(), bytes.size(), contextOf(type));
}

const clap_plugin_state_context_t kStateContext = {stateContextSave, stateContextLoad};

// --- presets (main thread) ----------------------------------------------------------------

bool CLAP_ABI presetFromLocation(const clap_plugin_t* p, uint32_t kind, const char* location, const char* loadKey) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::PresetLoad);
    bool ok = false;
    if (s->presets) {
        if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
            const int index = loadKey ? s->definition->findPreset(loadKey) : -1;
            ok = index >= 0 && s->presets->loadFactory(static_cast<size_t>(index));
        } else if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location) {
            ok = s->presets->loadFile(PresetLibrary::fromUtf8(location));
        }
    }
    if (!ok && s->hostPresetLoad && s->hostPresetLoad->on_error)
        s->hostPresetLoad->on_error(s->host, kind, location, loadKey, 0, "not a preset of this plugin");
    return ok;
}

const clap_plugin_preset_load_t kPresetLoad = {presetFromLocation};

// --- what the host says about the session (main thread) ------------------------------------

// Offline rendering, for a processor that declared kOfflineMode.
bool CLAP_ABI renderHardRealtime(const clap_plugin_t* p) {
    self(p)->expect(Thread::Main, Site::Render);
    return false;
}

bool CLAP_ABI renderSet(const clap_plugin_t* p, clap_plugin_render_mode mode) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::Render);
    if (mode != CLAP_RENDER_REALTIME && mode != CLAP_RENDER_OFFLINE) return false;
    s->engine->setOffline(mode == CLAP_RENDER_OFFLINE);
    return true;
}

const clap_plugin_render_t kRenderExt = {renderHardRealtime, renderSet};

// The plugin's own items in a host's context menu for a parameter: what the
// editor's menu offers too.
constexpr clap_id kMenuReset = 1;

bool CLAP_ABI menuPopulate(const clap_plugin_t* p, const clap_context_menu_target_t* target,
                           const clap_context_menu_builder_t* builder) {
    self(p)->expect(Thread::Main, Site::ContextMenu);
    if (!target || target->kind != CLAP_CONTEXT_MENU_TARGET_KIND_PARAM || !builder) return true;
    const int i = self(p)->editable(target->id);
    if (i < 0) return true;
    if (builder->supports && !builder->supports(builder, CLAP_CONTEXT_MENU_ITEM_ENTRY)) return true;
    const clap_context_menu_entry_t reset{"Reset to Default", true, kMenuReset};
    return builder->add_item(builder, CLAP_CONTEXT_MENU_ITEM_ENTRY, &reset);
}

bool CLAP_ABI menuPerform(const clap_plugin_t* p, const clap_context_menu_target_t* target, clap_id action) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::ContextMenu);
    if (!target || target->kind != CLAP_CONTEXT_MENU_TARGET_KIND_PARAM || action != kMenuReset) return false;
    const int i = s->editable(target->id);
    if (i < 0) return false;
    s->editWhole(static_cast<size_t>(i), hostDefault(s->table().at(static_cast<size_t>(i))));
    return true;
}

const clap_plugin_context_menu_t kContextMenu = {menuPopulate, menuPerform};

#if SUPPLE_WITH_GUI
void CLAP_ABI trackChanged(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::TrackInfo);
    s->refreshTrack();
}

const clap_plugin_track_info_t kTrackInfo = {trackChanged};
#endif

// Note names (Processor::noteNames), read fresh each time a host counts them.
uint32_t CLAP_ABI noteNameCount(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::NoteNames);
    s->noteNameList = s->currentNoteNames();
    return static_cast<uint32_t>(s->noteNameList.size());
}

bool CLAP_ABI noteNameGet(const clap_plugin_t* p, uint32_t index, clap_note_name_t* out) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::NoteNames);
    if (!out || index >= s->noteNameList.size()) return false;
    const NoteName& n = s->noteNameList[index];
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->name, sizeof(out->name), "%s", n.name.c_str());
    out->port = static_cast<int16_t>(n.port);
    out->key = static_cast<int16_t>(n.key);
    out->channel = static_cast<int16_t>(n.channel);
    return true;
}

const clap_plugin_note_name_t kNoteName = {noteNameCount, noteNameGet};

// The host's tunings are read as they are needed; nothing to do when the
// pool changes.
void CLAP_ABI tuningChanged(const clap_plugin_t* p) { self(p)->expect(Thread::Main, Site::Tuning); }

const clap_plugin_tuning_t kTuningExt = {tuningChanged};

const clap_plugin_state_t kState = {stateSave, stateLoad};

// --- gui -------------------------------------------------------------------------------------

#if SUPPLE_WITH_GUI

#if defined(_WIN32)
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* kWindowApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
#endif

bool CLAP_ABI guiIsApiSupported(const clap_plugin_t*, const char* api, bool isFloating) {
    return !isFloating && api && !std::strcmp(api, kWindowApi);
}

bool CLAP_ABI guiPreferredApi(const clap_plugin_t*, const char** api, bool* isFloating) {
    *api = kWindowApi;
    *isFloating = false;
    return true;
}

bool CLAP_ABI guiCreate(const clap_plugin_t* p, const char* api, bool isFloating) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::GuiCreate);
    if (!guiIsApiSupported(p, api, isFloating)) return false;
    if (!s->editor) {
        s->editor = std::make_unique<gui::Gui>(*s, s->info().name.c_str());
        s->editor->setPlatformScale(s->platformScale);
        s->editor->setUserScale(s->editorScale);
    }
    if (!s->editor->create()) {
        s->editor.reset();
        return false;
    }
    s->engine->telemetry().clear();  // a previous editor's unread frames
    s->engine->setMetering(true);
    s->startTimer();
    return true;
}

void CLAP_ABI guiDestroy(const clap_plugin_t* p) {
    self(p)->expect(Thread::Main, Site::GuiDestroy);
    self(p)->closeEditor();
}

bool CLAP_ABI guiSetScale(const clap_plugin_t* p, double scale) {
#if defined(__APPLE__)
    // Cocoa sizes are logical points; the host must not scale us as well.
    (void)p;
    (void)scale;
    return false;
#else
    Plugin* s = self(p);
    if (!std::isfinite(scale) || scale <= 0.0) return false;
    s->platformScale = scale;
    if (s->editor) s->editor->setPlatformScale(scale);
    return true;
#endif
}

bool CLAP_ABI guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    Plugin* s = self(p);
    if (!s->editor) return false;
    s->editor->size(*w, *h);
    return true;
}

bool CLAP_ABI guiCanResize(const clap_plugin_t*) { return true; }

bool CLAP_ABI guiResizeHints(const clap_plugin_t* p, clap_gui_resize_hints_t* hints) {
    Plugin* s = self(p);
    if (!s->editor) return false;
    uint32_t w = 0, h = 0;
    s->editor->aspect(w, h);
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = w;
    hints->aspect_ratio_height = h;
    return true;
}

bool CLAP_ABI guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    Plugin* s = self(p);
    return s->editor && s->editor->adjustSize(*w, *h);
}

bool CLAP_ABI guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::GuiSize);
    return s->editor && s->editor->setSize(w, h);
}

bool CLAP_ABI guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::GuiParent);
    if (!s->editor || !window) return false;
#if defined(_WIN32) || defined(__APPLE__)
    const uintptr_t parent = reinterpret_cast<uintptr_t>(window->ptr);
#else
    const uintptr_t parent = static_cast<uintptr_t>(window->x11);
#endif
    const bool ok = s->editor->setParent(parent);
    // A wrapper without clap.timer-support (clap-wrapper's AUv2 refuses every
    // timer) would leave a correct but frozen editor. Drive it ourselves.
    if (ok && s->timerId == CLAP_INVALID_ID) s->editor->startFallbackTimer(kTimerMs / 1000.0);
    return ok;
}

bool CLAP_ABI guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void CLAP_ABI guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool CLAP_ABI guiShow(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::GuiShow);
    if (!s->editor) return false;
    s->engine->telemetry().clear();
    const bool shown = s->editor->show();
    s->engine->setMetering(shown);
    if (shown) {
        s->startTimer();
        if (s->timerId == CLAP_INVALID_ID) s->editor->startFallbackTimer(kTimerMs / 1000.0);
    }
    return shown;
}

bool CLAP_ABI guiHide(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::GuiHide);
    s->engine->setMetering(false);
    s->stopTimer();
    if (!s->editor) return false;
    s->editor->stopFallbackTimer();
    s->editor->editor().focusLost();  // no gesture may outlive the window being visible
    return s->editor->hide();
}

const clap_plugin_gui_t kGui = {guiIsApiSupported, guiPreferredApi, guiCreate,       guiDestroy,
                                guiSetScale,       guiGetSize,      guiCanResize,    guiResizeHints,
                                guiAdjustSize,     guiSetSize,      guiSetParent,    guiSetTransient,
                                guiSuggestTitle,   guiShow,         guiHide};

#endif  // SUPPLE_WITH_GUI

// The editor's timer, and the layer's.
void CLAP_ABI timerTick(const clap_plugin_t* p, clap_id id) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::Timer);
#if SUPPLE_WITH_GUI
    if (s->editor && id == s->timerId) s->editor->tick();
#endif
    if (s->layer && id == s->layerTimerId) s->layer->tick();
}

const clap_plugin_timer_support_t kTimer = {timerTick};

// --- lifecycle ---------------------------------------------------------------------------------

bool CLAP_ABI pluginInit(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->hostParams = s->hostExtension<const clap_host_params_t>(CLAP_EXT_PARAMS);
    s->hostLatency = s->hostExtension<const clap_host_latency_t>(CLAP_EXT_LATENCY);
    s->hostTimer = s->hostExtension<const clap_host_timer_support_t>(CLAP_EXT_TIMER_SUPPORT);
    s->hostGui = s->hostExtension<const clap_host_gui_t>(CLAP_EXT_GUI);
    s->hostTail = s->hostExtension<const clap_host_tail_t>(CLAP_EXT_TAIL);
    s->hostVoiceInfo = s->hostExtension<const clap_host_voice_info_t>(CLAP_EXT_VOICE_INFO);
    s->hostState = s->hostExtension<const clap_host_state_t>(CLAP_EXT_STATE);
    s->hostPresetLoad = s->hostExtension<const clap_host_preset_load_t>(CLAP_EXT_PRESET_LOAD);
    if (!s->hostPresetLoad) s->hostPresetLoad = s->hostExtension<const clap_host_preset_load_t>(CLAP_EXT_PRESET_LOAD_COMPAT);
    s->hostLog = s->hostExtension<const clap_host_log_t>(CLAP_EXT_LOG);
    s->hostThreadCheck = s->hostExtension<const clap_host_thread_check_t>(CLAP_EXT_THREAD_CHECK);
    s->expect(Thread::Main, Site::Init);
    s->hostContextMenu = s->hostExtension<const clap_host_context_menu_t>(CLAP_EXT_CONTEXT_MENU);
    if (!s->hostContextMenu)
        s->hostContextMenu = s->hostExtension<const clap_host_context_menu_t>(CLAP_EXT_CONTEXT_MENU_COMPAT);
    s->hostTrackInfo = s->hostExtension<const clap_host_track_info_t>(CLAP_EXT_TRACK_INFO);
    if (!s->hostTrackInfo) s->hostTrackInfo = s->hostExtension<const clap_host_track_info_t>(CLAP_EXT_TRACK_INFO_COMPAT);
    if (s->namesNotes()) s->hostNoteName = s->hostExtension<const clap_host_note_name_t>(CLAP_EXT_NOTE_NAME);
    if (s->info().tuning) {
        const auto* registry = s->hostExtension<const clap_host_event_registry_t>(CLAP_EXT_EVENT_REGISTRY);
        uint16_t space = UINT16_MAX;
        if (registry && registry->query && registry->query(s->host, CLAP_EXT_TUNING, &space)) s->tuningSpace = space;
        s->tuning.attach(s->host, s->hostExtension<const clap_host_tuning_t>(CLAP_EXT_TUNING));
        s->engine->setTuning(&s->tuning);
    }
    s->hostAudioPorts = s->hostExtension<const clap_host_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
    s->hostNotePorts = s->hostExtension<const clap_host_note_ports_t>(CLAP_EXT_NOTE_PORTS);
#if SUPPLE_WITH_GUI
    s->refreshTrack();  // the host may never say it changed
#endif
    s->createPresets();
#if SUPPLE_PRODUCT_LAYER
    s->layer = product::makeLayer(*s);
    if (s->layer && s->hostTimer && s->hostTimer->register_timer) {
        clap_id id = CLAP_INVALID_ID;
        if (s->hostTimer->register_timer(s->host, kLayerTimerMs, &id)) s->layerTimerId = id;
    }
#endif
    return true;
}

void CLAP_ABI pluginDestroy(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::Destroy);
    if (s->layerTimerId != CLAP_INVALID_ID && s->hostTimer && s->hostTimer->unregister_timer)
        s->hostTimer->unregister_timer(s->host, s->layerTimerId);
    s->layerTimerId = CLAP_INVALID_ID;
    s->layer.reset();  // first: it may reach the rest until it goes
    s->tuning.detach();
#if SUPPLE_WITH_GUI
    // A host should destroy the editor first; not all do, and a timer left
    // pointing at freed memory is not survivable.
    s->closeEditor();
#endif
    delete s;
}

bool CLAP_ABI pluginActivate(const clap_plugin_t* p, double sampleRate, uint32_t, uint32_t maxFrames) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::Activate);
    if (!s->engine->activate(sampleRate, static_cast<int>(std::max<uint32_t>(1, maxFrames)),
                             s->info().audioInput() ? s->channels : 0, s->channels))
        return false;
    const int latency = s->engine->latency();
    // Telling the host during activate() is the only time it is allowed.
    if (s->reportedLatency >= 0 && latency != s->reportedLatency && s->hostLatency && s->hostLatency->changed)
        s->hostLatency->changed(s->host);
    s->reportedLatency = latency;
    s->restartOutstanding = false;
    // Whatever the host heard before, it hears each output again.
    s->resendOutputs = true;
    s->restartWanted.store(false, std::memory_order_relaxed);
    s->active = true;
    if (s->layer) s->layer->activated();
    return true;
}

void CLAP_ABI pluginDeactivate(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::Deactivate);
    s->engine->deactivate();
    s->active = false;
    // A redefinition waits for this: parameters change only while inactive.
    if (s->pendingDefinition) s->swapDefinition();
    if (s->layer) s->layer->deactivated();
}

bool CLAP_ABI pluginStartProcessing(const clap_plugin_t* p) SUPPLE_REALTIME {
    self(p)->expect(Thread::Audio, Site::StartProcessing);
    return true;
}
void CLAP_ABI pluginStopProcessing(const clap_plugin_t* p) SUPPLE_REALTIME {
    self(p)->expect(Thread::Audio, Site::StopProcessing);
}

void CLAP_ABI pluginReset(const clap_plugin_t* p) SUPPLE_REALTIME {
    self(p)->expect(Thread::Audio, Site::Reset);
    self(p)->engine->reset();
}

template <class T>
T* const* channelsOf(const clap_audio_buffer_t& b) {
    if constexpr (std::is_same_v<T, float>) return b.data32;
    else return b.data64;
}

// Hands the engine every bus the host connected, in one precision. False
// when the host broke the ports' contract.
template <class T, class Source>
bool processAs(Plugin* s, const clap_process_t* process, Source& events, const Transport* t) SUPPLE_REALTIME {
    const PluginInfo& info = s->info();
    const uint32_t width = static_cast<uint32_t>(s->channels);
    AudioBuses<T> io;
    clap_audio_buffer_t& out = process->audio_outputs[0];
    io.out = channelsOf<T>(out);
    if (info.audioInput()) {
        const clap_audio_buffer_t& in = process->audio_inputs[0];
        if (in.channel_count < width || !channelsOf<T>(in)) return false;
        io.in = channelsOf<T>(in);
    }
    // A bus the host left unconnected, or narrower than it was told, is
    // silence in and nowhere out.
    if (info.sidechain) {
        if (sidechainPort(info) < process->audio_inputs_count) {
            const clap_audio_buffer_t& sc = process->audio_inputs[sidechainPort(info)];
            if (sc.channel_count >= width) io.sidechain = channelsOf<T>(sc);
        }
    }
    // Aux buses beyond what fits here are left untouched (and silent).
    constexpr uint32_t kMaxAux = 64;
    T* const* aux[kMaxAux] = {};
    const uint32_t auxCount = std::min(static_cast<uint32_t>(info.auxCount()), kMaxAux);
    if (auxCount > 0) {
        for (uint32_t k = 0; k < auxCount; ++k) {
            if (1 + k >= process->audio_outputs_count) break;
            clap_audio_buffer_t& bus = process->audio_outputs[1 + k];
            bus.constant_mask = 0;
            if (bus.channel_count >= width) aux[k] = channelsOf<T>(bus);
        }
        io.aux = aux;
    }
    s->engine->process(io, static_cast<int>(process->frames_count), events, t);
    return true;
}

clap_process_status CLAP_ABI pluginProcess(const clap_plugin_t* p, const clap_process_t* process) SUPPLE_REALTIME {
    Plugin* s = self(p);
    s->expect(Thread::Audio, Site::Process);
    ClapEvents events(s->table(), s->info().takesNotes(), process->in_events, s->tuningSpace);
    // Edits first, so the host records them in the block that plays them.
    s->drainEdits(process->out_events);

    Transport transport;
    const Transport* t = nullptr;
    if (const clap_event_transport_t* ct = process->transport) {
        transport.playing = (ct->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        transport.hasTempo = (ct->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0;
        transport.tempo = ct->tempo;
        transport.hasBeats = (ct->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
        transport.beats = static_cast<double>(ct->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        transport.barStart = static_cast<double>(ct->bar_start) / CLAP_BEATTIME_FACTOR;
        transport.timeSigNumerator = ct->tsig_num;
        transport.timeSigDenominator = ct->tsig_denom;
        t = &transport;
    }

    if (s->info().noteEffect()) {
        // Notes in, notes out, no audio: still split at every event.
        AudioBuses<double> none;
        s->engine->process(none, static_cast<int>(process->frames_count), events, t);
    } else if (process->audio_outputs_count < 1 || (s->info().audioInput() && process->audio_inputs_count < 1)) {
        s->report(kBadBuses);
        s->engine->flush(events);
    } else {
        clap_audio_buffer_t& out = process->audio_outputs[0];
        // Bus widths are fixed by the selected configuration and activate();
        // a mismatch is a host contract violation, not a cue to reallocate.
        if (static_cast<int>(out.channel_count) < s->channels) {
            s->report(kBadBuses);
            return CLAP_PROCESS_ERROR;
        }
        // Every sample is written, and nothing establishes that they are
        // equal: a stale hint would have the host read sample 0 throughout.
        out.constant_mask = 0;
        // The ports require one precision throughout; mixed is refused.
        bool ok = true;
        if (out.data32)
            ok = processAs<float>(s, process, events, t);
        else if (out.data64)
            ok = processAs<double>(s, process, events, t);
        else
            s->engine->flush(events);
        if (!ok) {
            s->report(kBadBuses);
            return CLAP_PROCESS_ERROR;
        }
    }
    s->afterAudioWork(process->out_events, process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension(const clap_plugin_t* p, const char* id) {
    const Plugin& s = *self(p);
    const PluginInfo& info = s.info();
    if (s.layer)
        if (const void* own = s.layer->extension(id)) return own;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (info.audioOutput()) {
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG)) return &kAudioPortsConfig;
        if (!std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS) ||
            !std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT))
            return &kConfigurablePorts;
    }
    if (anyLayout(info, isSurround) && (!std::strcmp(id, CLAP_EXT_SURROUND) || !std::strcmp(id, CLAP_EXT_SURROUND_COMPAT)))
        return &kSurround;
    if (anyLayout(info, isAmbisonic) &&
        (!std::strcmp(id, CLAP_EXT_AMBISONIC) || !std::strcmp(id, CLAP_EXT_AMBISONIC_COMPAT)))
        return &kAmbisonic;
    if ((info.takesNotes() || info.sendsNotes()) && !std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (voiceInfo(s) && !std::strcmp(id, CLAP_EXT_VOICE_INFO)) return &kVoiceInfoExt;
    if (info.takesNotes() && !std::strcmp(id, CLAP_PLUGIN_AS_VST3)) return &kAsVst3;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_STATE_CONTEXT)) return &kStateContext;
    if (info.presets && (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)))
        return &kPresetLoad;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTail;
    if (kRender && !std::strcmp(id, CLAP_EXT_RENDER)) return &kRenderExt;
    if (!std::strcmp(id, CLAP_EXT_CONTEXT_MENU) || !std::strcmp(id, CLAP_EXT_CONTEXT_MENU_COMPAT)) return &kContextMenu;
    if (s.namesNotes() && !std::strcmp(id, CLAP_EXT_NOTE_NAME)) return &kNoteName;
    if (info.tuning && !std::strcmp(id, CLAP_EXT_TUNING)) return &kTuningExt;
#if SUPPLE_WITH_GUI
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
    if (!std::strcmp(id, CLAP_EXT_TRACK_INFO) || !std::strcmp(id, CLAP_EXT_TRACK_INFO_COMPAT)) return &kTrackInfo;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimer;
#else
    if (s.layer && !std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimer;
#endif
    return nullptr;
}

void CLAP_ABI pluginOnMainThread(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->expect(Thread::Main, Site::OnMainThread);
    s->callbackRequested.store(false, std::memory_order_release);
    s->engine->properties().collect();
    s->logReports();
    if (s->namesChanged.exchange(false, std::memory_order_acq_rel)) s->checkNoteNames();
    if (s->restartWanted.exchange(false, std::memory_order_acq_rel)) s->requestRestart();
    if (s->voicesChanged.exchange(false, std::memory_order_acq_rel) && s->hostVoiceInfo && s->hostVoiceInfo->changed)
        s->hostVoiceInfo->changed(s->host);
    if (s->flushWanted.exchange(false, std::memory_order_acq_rel)) {
        s->flushRequested.store(false, std::memory_order_release);
        s->requestFlush();
    }
    if (s->layer && s->layerCallback.exchange(false, std::memory_order_acq_rel)) s->layer->onMainThread();
}

// --- factory ----------------------------------------------------------------------------------

// A plugin's CLAP descriptor and the text it points to.
struct Descriptor {
    DefinitionPtr definition;
    std::vector<std::string> text;  // id, name, vendor, url, version, description
    std::vector<const char*> features;
    clap_plugin_descriptor_t clap{};

    explicit Descriptor(DefinitionPtr d) : definition(std::move(d)) {
        const PluginInfo& i = definition->info();
        text = {i.id, i.name, i.vendor, i.url, i.version, i.description};
        for (const std::string& f : i.features) features.push_back(f.c_str());
        features.push_back(nullptr);
        clap = {CLAP_VERSION_INIT, text[0].c_str(), text[1].c_str(), text[2].c_str(), text[3].c_str(), "", "",
                text[4].c_str(), text[5].c_str(), features.data()};
    }
};

// What the factory has listed (main thread). A descriptor lives as long as
// the module: hosts keep the pointers.
struct Listing {
    std::vector<std::unique_ptr<Descriptor>> all;  // every one handed out
    std::vector<const Descriptor*> current;        // the catalog as last listed

    // The catalog now. A plugin that changed gets a new descriptor; the old
    // one stays valid.
    void refresh() {
        current.clear();
        for (DefinitionPtr& d : catalog().list())
            if (usable(*d)) current.push_back(&descriptorFor(std::move(d)));
    }
    // A plugin whose definition breaks the rules is not offered. (A product
    // with tables cannot build with one.)
    static bool usable(const Definition& d) { return d.problems().empty(); }
    const Descriptor& descriptorFor(DefinitionPtr d) {
        for (auto& existing : all)
            if (existing->definition == d) return *existing;
        all.push_back(std::make_unique<Descriptor>(std::move(d)));
        return *all.back();
    }
};

Listing& listing() {
    static Listing instance;
    return instance;
}

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t*) {
    listing().refresh();
    return static_cast<uint32_t>(listing().current.size());
}

const clap_plugin_descriptor_t* CLAP_ABI factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
    if (listing().current.empty()) listing().refresh();
    return index < listing().current.size() ? &listing().current[index]->clap : nullptr;
}

const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (!host || !id) return nullptr;
    DefinitionPtr definition = catalog().find(id);
    if (!definition || !Listing::usable(*definition)) return nullptr;
    const Descriptor& descriptor = listing().descriptorFor(definition);
    auto* s = new Plugin(std::move(definition));
    s->host = host;
    s->clap.desc = &descriptor.clap;
    s->clap.plugin_data = s;
    s->clap.init = pluginInit;
    s->clap.destroy = pluginDestroy;
    s->clap.activate = pluginActivate;
    s->clap.deactivate = pluginDeactivate;
    s->clap.start_processing = pluginStartProcessing;
    s->clap.stop_processing = pluginStopProcessing;
    s->clap.reset = pluginReset;
    s->clap.process = pluginProcess;
    s->clap.get_extension = pluginExtension;
    s->clap.on_main_thread = pluginOnMainThread;
    return &s->clap;
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

}  // namespace

Layer* Layer::of(const clap_plugin_t* plugin) {
    return plugin && plugin->plugin_data ? self(plugin)->layer.get() : nullptr;
}

}  // namespace supple::plugin

supple::gui::EditorHost* supple_test_editor_host(const clap_plugin_t* plugin) {
#if SUPPLE_WITH_GUI
    return plugin ? supple::plugin::self(plugin) : nullptr;
#else
    (void)plugin;
    return nullptr;
#endif
}

void supple_test_set_metering(const clap_plugin_t* plugin, bool on) {
    if (plugin) supple::plugin::self(plugin)->engine->setMetering(on);
}

uint64_t supple_test_wrong_threads(const clap_plugin_t* plugin) {
    return plugin ? supple::plugin::self(plugin)->wrongThreads.load() : 0;
}

uint32_t supple_test_reports(const clap_plugin_t* plugin) {
    return plugin ? supple::plugin::self(plugin)->reports.load() : 0;
}

bool supple_test_offline(const clap_plugin_t* plugin) {
    return plugin && supple::plugin::self(plugin)->engine->offline();
}

double supple_test_retuning(const clap_plugin_t* plugin, int key, int channel) {
    return plugin ? supple::plugin::self(plugin)->tuning.retuning(key, channel, 0) : 0.0;
}

bool supple_entry_init(const char*) { return true; }
void supple_entry_deinit() {}
const void* supple_entry_get_factory(const char* factoryId) {
    if (!factoryId) return nullptr;
    if (!std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID)) return &supple::plugin::kFactory;
    if (!std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
        !std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT)) {
        // Offered while any plugin in the catalog has presets.
        for (const supple::DefinitionPtr& d : supple::catalog().list())
            if (d->info().presets) return supple::plugin::presetDiscoveryFactory();
    }
    return nullptr;
}
