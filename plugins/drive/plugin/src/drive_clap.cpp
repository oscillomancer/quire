// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Drive — CLAP plugin implementation.
//
// Built as a static library exposing clap_init / clap_deinit / clap_get_factory;
// clap-wrapper assembles it into CLAP, VST3 and AUv2 from the same object code,
// so all three formats are the same DSP with the same parameter IDs and the
// same saved state.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "DriveParams.h"
#include "drive/processor.h"
#include "drive_entry.h"
#include "supple/clap/plugin_support.h"

#if DRIVE_WITH_GUI
#include "Gui.h"
#include "Telemetry.h"
#endif

namespace {

// The parameter table lives in DriveParams.h so that the editor renders
// names, ranges and value text from the same source the host sees.
using namespace supple::drive;

// ---------------------------------------------------------------------------

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DISTORTION,
                           CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT,
                                        "com.suppleaudio.drive",
                                        kDisplayName,
                                        "Supple Audio",
                                        "",
                                        "",
                                        "",
                                        "0.1.0",
                                        "Stereo gain, balance, width and bass-mono drive.",
                                        kFeatures};

// ---------------------------------------------------------------------------

struct Plug
#if DRIVE_WITH_GUI
    : supple::drive::gui::EditorHost
#endif
{
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;

    supple::drive::Processor processor;
    supple::clapx::ParamStore store{kParams};
    supple::clapx::FlatSemantic semantic{
        patchType(),
        store,
        "com.suppleaudio.drive",
        "ADAA saturation with oversampling and reference RMS compensation.",
        "",
        0x51445231};
    uint64_t seenGeneration = 0;
    Values audioValues = defaults();
    std::atomic<double> srAtomic{48000.0};
    int channels = 2;
    int preparedChannels = 0;
    uint32_t meterSamples = 0;
    std::atomic<bool> active{false};
    std::atomic<bool> restartRequested{false};
    std::atomic<int> preparedOversampling{-1};
    std::atomic<int> latencyAtomic{0};
    // Scratch for process(), sized once from the selected mono/stereo port
    // configuration during activation and never resized on the audio thread.
    std::vector<float*> ptrs32;
    std::vector<double*> ptrs64;

    Plug() {
        semantic.onApplied = [this](const auto&, const auto&) {
            requestQualityRestart();
            if (hostParams && hostParams->rescan)
                hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        };
    }
    void sizeScratch(int nch) {
        ptrs32.resize((size_t)nch);
        ptrs64.resize((size_t)nch);
    }

    void requestQualityRestart() {
        if (active.load(std::memory_order_acquire) &&
            static_cast<int>(store.get(kOversampling)) !=
                preparedOversampling.load(std::memory_order_relaxed) &&
            !restartRequested.exchange(true, std::memory_order_acq_rel) && host &&
            host->request_restart)
            host->request_restart(host);
    }
    double pv(uint32_t id) const { return store.get(id); }
    void sv(uint32_t id, double v) {
        store.set(id, v);
        if (id == kOversampling) requestQualityRestart();
    }
    void loadDefaults() { store.resetToDefaults(); }
    void pushParams() {
        const auto gen = store.generation();
        if (gen != seenGeneration && store.readAll(audioValues)) {
            seenGeneration = gen;
            processor.setParams(audioValues);
            requestQualityRestart();
        }
    }
#if DRIVE_WITH_GUI
    supple::drive::gui::TelemetryRing telemetryRing;
    supple::drive::gui::ParamRing editRing;
    supple::drive::gui::GestureRing deferredGestureRing;
    supple::drive::gui::Gui* gui = nullptr;
    // Whether anything is looking. Read by the audio thread every chunk, so it
    // is atomic; written by the main thread from the editor's lifecycle.
    std::atomic<bool> telemetryOn{false};
    bool guiTimerRunning = false;
    clap_id guiTimerId = 0;
    int telemetryChunk = 192;
    // Main-thread gesture producer state.
    bool gestureQueued = false;
    uint32_t gestureId = 0;
    // Audio-thread retry state for one compact deferred gesture.
    bool deferredGestureActive = false;
    supple::drive::gui::ParamGesture deferredGesture{};
    int deferredGesturePhase = 0;
    std::atomic<bool> editRetryOnMain{false};

#endif
    void captureTelemetry(const clap_output_events_t* out, uint32_t frame) {
        const auto t = processor.takeTelemetry();
        const double values[] = {t.inputPeakDb, t.outputPeakDb};
        const uint32_t ids[] = {kInputMeter, kOutputMeter};
        for (int i = 0; i < 2; ++i) {
            sv(ids[i], values[i]);
            clap_event_param_value_t event{};
            event.header.size = sizeof(event);
            event.header.time = frame;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.param_id = ids[i];
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = pa::toClap(kParams[ids[i]], values[i]);
            if (out) out->try_push(out, &event.header);
        }
#if DRIVE_WITH_GUI
        if (telemetryOn.load(std::memory_order_relaxed)) {
            supple::drive::gui::Frame f{};
            f.inputPeakDb = static_cast<float>(t.inputPeakDb);
            f.outputPeakDb = static_cast<float>(t.outputPeakDb);

            telemetryRing.push(f);
        }
#endif
    }
    template <typename T>
    void runChunks(std::vector<T*>& v, int n, const clap_output_events_t* out, uint32_t baseFrame) {
        const uint32_t period =
            static_cast<uint32_t>(std::ceil(srAtomic.load(std::memory_order_relaxed) * 0.05));
        int offset = 0;
        while (offset < n) {
            int count = std::min(n - offset, static_cast<int>(period - meterSamples));
            processor.process(v.data(), count, true);
            meterSamples += static_cast<uint32_t>(count);
            if (meterSamples == period) {
                captureTelemetry(out, baseFrame + static_cast<uint32_t>(offset + count - 1));
                meterSamples = 0;
            }
            for (auto& ptr : v) ptr += count;
            offset += count;
        }
        for (auto& ptr : v) ptr -= n;
    }
#if DRIVE_WITH_GUI

    // --- EditorHost ---------------------------------------------------------

    double paramValue(uint32_t id) const override { return pv(id); }

    void beginEdit(uint32_t id) override {
        gestureId = id;
        gestureQueued = !deferredGestureRing.pending() && editRing.free() >= 3 &&
                        queueEdit({id, pv(id), supple::drive::gui::ParamEdit::kBegin}, true);
        if (!gestureQueued && hostParams && hostParams->request_flush)
            hostParams->request_flush(host);
    }

    void setParam(uint32_t id, double v) override {
        const ParamDef* d = findParam(id);
        if (!d || (d->flags & pa::kReadonly)) return;
        const double c = std::max(d->min, std::min(d->max, v));
        // Apply locally at once so the knob tracks the mouse even with the
        // transport stopped; the ring is what tells the host.
        sv(id, c);
        if (gestureQueued && id == gestureId)
            queueEdit({id, c, supple::drive::gui::ParamEdit::kValue});
    }

    void endEdit(uint32_t id) override {
        if (gestureQueued && id == gestureId) {
            // The begin reserved these two slots for the exact final value/end.
            queueEdit({id, pv(id), supple::drive::gui::ParamEdit::kValue}, true);
            queueEdit({id, pv(id), supple::drive::gui::ParamEdit::kEnd}, true);
        } else {
            deferredGestureRing.push({id, pv(id)});
            if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
        }
        gestureQueued = false;
    }

    supple::drive::gui::TelemetryRing& telemetry() override { return telemetryRing; }

    int latencySamples() const override { return latencyAtomic.load(std::memory_order_relaxed); }

    double sampleRate() const override { return srAtomic.load(std::memory_order_relaxed); }

    bool queueEdit(const supple::drive::gui::ParamEdit& e, bool boundary = false) {
        if (!boundary && e.kind == supple::drive::gui::ParamEdit::kValue && editRing.free() <= 2)
            return false;
        if (!editRing.push(e)) return false;
        // Without this the host never sees the edit while the transport is
        // stopped, because process() is not being called.
        if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
        return true;
    }
#endif
};

// ---------------------------------------------------------------------------
// Extension: audio ports
// ---------------------------------------------------------------------------

uint32_t CLAP_ABI apCount(const clap_plugin_t*, bool) { return 1; }

bool CLAP_ABI apGet(const clap_plugin_t* p, uint32_t index, bool isInput,
                    clap_audio_port_info_t* info) {
    if (index != 0) return false;
    auto* self = (Plug*)p->plugin_data;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS |
                  CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
    info->channel_count = (uint32_t)self->channels;
    info->port_type = self->channels == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    // Both directions name their partner; CLAP in-place pairing is reciprocal.
    info->in_place_pair = 0;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {apCount, apGet};

// ---------------------------------------------------------------------------
// Extension: audio port configurations
// ---------------------------------------------------------------------------

constexpr clap_id kMonoConfig = 1;
constexpr clap_id kStereoConfig = 2;

uint32_t CLAP_ABI apcCount(const clap_plugin_t*) { return 2; }

bool CLAP_ABI apcGet(const clap_plugin_t*, uint32_t index, clap_audio_ports_config_t* config) {
    if (index >= 2) return false;
    std::memset(config, 0, sizeof(*config));
    const bool stereo = index == 1;
    config->id = stereo ? kStereoConfig : kMonoConfig;
    std::snprintf(config->name, sizeof(config->name), "%s", stereo ? "Stereo" : "Mono");
    config->input_port_count = 1;
    config->output_port_count = 1;
    config->has_main_input = true;
    config->main_input_channel_count = stereo ? 2u : 1u;
    config->main_input_port_type = stereo ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    config->has_main_output = true;
    config->main_output_channel_count = stereo ? 2u : 1u;
    config->main_output_port_type = stereo ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    return true;
}

bool CLAP_ABI apcSelect(const clap_plugin_t* p, clap_id id) {
    auto* self = (Plug*)p->plugin_data;
    if (self->active || (id != kMonoConfig && id != kStereoConfig)) return false;
    self->channels = id == kMonoConfig ? 1 : 2;
    return true;
}

const clap_plugin_audio_ports_config_t kAudioPortsConfig = {apcCount, apcGet, apcSelect};

// ---------------------------------------------------------------------------
// Extension: latency
// ---------------------------------------------------------------------------

uint32_t CLAP_ABI latGet(const clap_plugin_t* p) {
    return static_cast<uint32_t>(
        static_cast<Plug*>(p->plugin_data)->latencyAtomic.load(std::memory_order_relaxed));
}
const clap_plugin_latency_t kLatency = {latGet};

// ---------------------------------------------------------------------------
// Extension: params
// ---------------------------------------------------------------------------

uint32_t CLAP_ABI paramCount(const clap_plugin_t*) {
    return supple::clapx::ClapParams::count(kParams);
}
bool CLAP_ABI paramInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* o) {
    return supple::clapx::ClapParams::info(kParams, i, o);
}
bool CLAP_ABI paramGetValue(const clap_plugin_t* p, clap_id id, double* o) {
    return static_cast<Plug*>(p->plugin_data)->store.getClap(id, *o);
}
bool CLAP_ABI paramValueToText(const clap_plugin_t*, clap_id id, double v, char* o, uint32_t n) {
    return supple::clapx::ClapParams::valueToText(kParams, id, v, o, n);
}
bool CLAP_ABI paramTextToValue(const clap_plugin_t*, clap_id id, const char* t, double* o) {
    return supple::clapx::ClapParams::textToValue(kParams, id, t, o);
}
void applyEvent(Plug* self, const clap_event_header_t* h) {
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE &&
        h->size >= sizeof(clap_event_param_value_t)) {
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
        self->store.setClap(e->param_id, e->value);
    }
}

// Edits made in the editor reach the host from here and from process(). Both
// are required: process() covers playback, paramFlush() covers a stopped
// transport, and the host decides which one it calls.
// Returns true if any edit was drained, which means values[] has moved and the
// processor has to be told. Forgetting that is silent and total: the knob tracks
// the mouse, the host records the automation, and the audio never changes.
#if DRIVE_WITH_GUI
bool emitEdit(const clap_output_events_t* out, const supple::drive::gui::ParamEdit& e,
              uint32_t frame) {
    if (!out) return false;
    if (e.kind == supple::drive::gui::ParamEdit::kValue) {
        clap_event_param_value_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = frame;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = e.id;
        ev.cookie = nullptr;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = pa::toClap(*findParam(e.id), e.value);
        return out->try_push(out, &ev.header);
    }
    clap_event_param_gesture_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.time = frame;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = e.kind == supple::drive::gui::ParamEdit::kBegin
                         ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                         : CLAP_EVENT_PARAM_GESTURE_END;
    ev.header.flags = 0;
    ev.param_id = e.id;
    return out->try_push(out, &ev.header);
}

void requestEditRetry(Plug* self) {
    // request_flush is explicitly forbidden on the audio thread. Hop through
    // the CLAP main-thread callback instead; request_callback itself is
    // thread-safe and coalescing avoids callback storms under host backpressure.
    if (!self->editRetryOnMain.exchange(true, std::memory_order_acq_rel) && self->host &&
        self->host->request_callback)
        self->host->request_callback(self->host);
}
#endif

bool drainEdits(Plug* self, const clap_output_events_t* out, uint32_t frame) {
#if DRIVE_WITH_GUI
    bool any = false;
    supple::drive::gui::ParamEdit e;
    while (self->editRing.peek(e)) {
        any = true;
        if (!emitEdit(out, e, frame)) {
            requestEditRetry(self);
            return any;
        }
        self->editRing.consume();
    }

    for (;;) {
        if (!self->deferredGestureActive) {
            if (!self->deferredGestureRing.peek(self->deferredGesture)) break;
            self->deferredGestureActive = true;
            self->deferredGesturePhase = 0;
        }
        const auto kind = self->deferredGesturePhase == 0   ? supple::drive::gui::ParamEdit::kBegin
                          : self->deferredGesturePhase == 1 ? supple::drive::gui::ParamEdit::kValue
                                                            : supple::drive::gui::ParamEdit::kEnd;
        e = {self->deferredGesture.id, self->deferredGesture.value, kind};
        any = true;
        if (!emitEdit(out, e, frame)) {
            requestEditRetry(self);
            return any;
        }
        if (++self->deferredGesturePhase == 3) {
            self->deferredGestureRing.consume();
            self->deferredGestureActive = false;
        }
    }
    return any;
#else
    (void)self;
    (void)out;
    (void)frame;
    return false;
#endif
}

void CLAP_ABI paramFlush(const clap_plugin_t* p, const clap_input_events_t* in,
                         const clap_output_events_t* out) {
    auto* self = (Plug*)p->plugin_data;
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) applyEvent(self, in->get(in, i));
    drainEdits(self, out, 0);
    self->pushParams();
}

const clap_plugin_params_t kParamsExt = {paramCount,       paramInfo,        paramGetValue,
                                         paramValueToText, paramTextToValue, paramFlush};

// ---------------------------------------------------------------------------
// Extension: state
// ---------------------------------------------------------------------------
//
// Versioned id/value pairs rather than a raw struct dump, so that reordering or
// adding parameters later cannot silently misread an old session.

bool CLAP_ABI stateSave(const clap_plugin_t* p, const clap_ostream_t* o) {
    return static_cast<Plug*>(p->plugin_data)->semantic.saveState(o);
}
bool CLAP_ABI stateLoad(const clap_plugin_t* p, const clap_istream_t* i) {
    return static_cast<Plug*>(p->plugin_data)->semantic.loadState(i);
}
const clap_plugin_state_t kState = {stateSave, stateLoad};
supple::clapx::FlatSemantic* resolveSemantic(const clap_plugin_t* p) {
    return &static_cast<Plug*>(p->plugin_data)->semantic;
}
bool CLAP_ABI describeSemantic(const clap_plugin_t* p, uint32_t detail, const clap_ostream_t* o) {
    const auto* self = static_cast<Plug*>(p->plugin_data);
    Values values{};
    const auto current = self->store.snapshot();
    std::copy(current.values.begin(), current.values.end(), values.begin());
    const auto text = describe(values, detail != CLAP_SUPPLE_PATCH_DETAIL_FULL);
    size_t offset = 0;
    while (offset < text.size()) {
        auto n = o->write(o, text.data() + offset, text.size() - offset);
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}
const clap_plugin_supple_semantic_params_t* semanticExtension() {
    static const auto ext = [] {
        auto e = *supple::clapx::FlatSemantic::extension<resolveSemantic>();
        e.describe = describeSemantic;
        return e;
    }();
    return &ext;
}
// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

#if DRIVE_WITH_GUI
void stopGuiTimer(Plug* self);  // defined with the rest of the GUI extension
#endif

bool CLAP_ABI plugInit(const clap_plugin_t* p) {
    auto* self = (Plug*)p->plugin_data;
    self->loadDefaults();
    if (self->host)
        self->hostParams =
            (const clap_host_params_t*)self->host->get_extension(self->host, CLAP_EXT_PARAMS);
    return true;
}

void CLAP_ABI plugDestroy(const clap_plugin_t* p) {
    auto* self = (Plug*)p->plugin_data;
#if DRIVE_WITH_GUI
    // A host is supposed to call gui->destroy() first, but not all do, and
    // leaving a timer pointing at freed memory is not a survivable bug.
    stopGuiTimer(self);
    delete self->gui;
    self->gui = nullptr;
#endif
    delete self;
}

bool CLAP_ABI plugActivate(const clap_plugin_t* p, double sr, uint32_t, uint32_t maxFrames) {
    auto* self = (Plug*)p->plugin_data;
    self->srAtomic.store(sr, std::memory_order_relaxed);
#if DRIVE_WITH_GUI
    // 250 Hz of telemetry at any session rate.
    self->telemetryChunk = std::max(1, (int)std::lround(sr / 250.0));
#endif
    self->pushParams();
    self->processor.prepare(sr, maxFrames, self->channels);
    self->preparedOversampling.store(static_cast<int>(self->pv(kOversampling)),
                                     std::memory_order_relaxed);
    self->restartRequested.store(false, std::memory_order_release);
    const int before =
        self->latencyAtomic.exchange(self->processor.latencySamples(), std::memory_order_relaxed);
    if (before != self->processor.latencySamples() && self->host) {
        const auto* latency = static_cast<const clap_host_latency_t*>(
            self->host->get_extension(self->host, CLAP_EXT_LATENCY));
        if (latency && latency->changed) latency->changed(self->host);
    }
    self->preparedChannels = self->channels;
    self->sizeScratch(self->channels);
    self->meterSamples = 0;
    self->seenGeneration = 0;
    self->pushParams();
    self->active = true;
    return true;
}

void CLAP_ABI plugDeactivate(const clap_plugin_t* p) { ((Plug*)p->plugin_data)->active = false; }

bool CLAP_ABI plugStartProcessing(const clap_plugin_t*) { return true; }
void CLAP_ABI plugStopProcessing(const clap_plugin_t*) {}

void CLAP_ABI plugReset(const clap_plugin_t* p) { ((Plug*)p->plugin_data)->processor.reset(); }

clap_process_status CLAP_ABI plugProcess(const clap_plugin_t* p,
                                         const clap_process_t* proc) SUPPLE_RT;
clap_process_status CLAP_ABI plugProcess(const clap_plugin_t* p, const clap_process_t* proc) {
    auto* self = (Plug*)p->plugin_data;
    const uint32_t nframes = proc->frames_count;
    const clap_input_events_t* in = proc->in_events;
    const uint32_t nev = in ? in->size(in) : 0;

    if (proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;

    const clap_audio_buffer_t& ib = proc->audio_inputs[0];
    clap_audio_buffer_t& ob = proc->audio_outputs[0];
    const uint32_t nch = std::min(ib.channel_count, ob.channel_count);
    if (nch == 0) return CLAP_PROCESS_CONTINUE;

    // Port selection happens while deactivated and the channel count is fixed
    // by activate(). A mismatch is a host-contract violation, not permission to
    // allocate or rebuild topology on the realtime thread.
    if ((int)nch != self->preparedChannels) return CLAP_PROCESS_ERROR;

    // Editor edits are drained before any audio is rendered, so a knob move
    // takes effect in the block you hear it in rather than the next one.
    if (drainEdits(self, proc->out_events, 0)) self->pushParams();

    self->pushParams();
    uint32_t ev = 0, frame = 0;
    bool dirty = false;

    while (frame < nframes) {
        // Apply every event scheduled at or before this frame, then process up
        // to the next one: sample-accurate automation rather than block-rate.
        while (ev < nev) {
            const clap_event_header_t* h = in->get(in, ev);
            if (h->time > frame) break;
            applyEvent(self, h);
            dirty = true;
            ++ev;
        }
        if (dirty) {
            self->pushParams();
            dirty = false;
        }

        uint32_t next = nframes;
        if (ev < nev) {
            const clap_event_header_t* h = in->get(in, ev);
            next = std::min(nframes, h->time);
        }
        const uint32_t n = next - frame;
        if (n == 0) continue;

        if (ob.data32 && ib.data32) {
            for (uint32_t c = 0; c < nch; ++c) {
                if (ob.data32[c] != ib.data32[c])
                    std::memcpy(ob.data32[c] + frame, ib.data32[c] + frame, n * sizeof(float));
                self->ptrs32[c] = ob.data32[c] + frame;
            }
            self->runChunks(self->ptrs32, (int)n, proc->out_events, frame);
        } else if (ob.data64 && ib.data64) {
            for (uint32_t c = 0; c < nch; ++c) {
                if (ob.data64[c] != ib.data64[c])
                    std::memcpy(ob.data64[c] + frame, ib.data64[c] + frame, n * sizeof(double));
                self->ptrs64[c] = ob.data64[c] + frame;
            }
            self->runChunks(self->ptrs64, (int)n, proc->out_events, frame);
        }
        frame = next;
    }
    return CLAP_PROCESS_CONTINUE;
}

// ---------------------------------------------------------------------------
// Extension: GUI
// ---------------------------------------------------------------------------
//
// Implemented once, here. clap-wrapper forwards clap_plugin_gui to VST3's
// IPlugView and to the AUv2 Cocoa view, and forwards clap.timer-support back
// the other way, so all three formats get the editor and its 60 Hz tick from
// this one implementation.
#if DRIVE_WITH_GUI

#if defined(_WIN32)
constexpr const char* kNativeApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* kNativeApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kNativeApi = CLAP_WINDOW_API_X11;
#endif

constexpr uint32_t kGuiTimerMs = 16;  // ~60 Hz

void startGuiTimer(Plug* self) {
    if (self->guiTimerRunning || !self->host) return;
    auto* ht = (const clap_host_timer_support_t*)self->host->get_extension(self->host,
                                                                           CLAP_EXT_TIMER_SUPPORT);
    if (ht && ht->register_timer && ht->register_timer(self->host, kGuiTimerMs, &self->guiTimerId))
        self->guiTimerRunning = true;
}

void stopGuiTimer(Plug* self) {
    if (!self->guiTimerRunning || !self->host) return;
    auto* ht = (const clap_host_timer_support_t*)self->host->get_extension(self->host,
                                                                           CLAP_EXT_TIMER_SUPPORT);
    if (ht && ht->unregister_timer) ht->unregister_timer(self->host, self->guiTimerId);
    self->guiTimerRunning = false;
}

bool CLAP_ABI guiIsApiSupported(const clap_plugin_t*, const char* api, bool isFloating) {
    return !isFloating && api && !std::strcmp(api, kNativeApi);
}

bool CLAP_ABI guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* isFloating) {
    *api = kNativeApi;
    *isFloating = false;
    return true;
}

bool CLAP_ABI guiCreate(const clap_plugin_t* p, const char* api, bool isFloating) {
    if (!guiIsApiSupported(p, api, isFloating)) return false;
    auto* self = (Plug*)p->plugin_data;
    if (!self->gui) self->gui = new supple::drive::gui::Gui(*self);
    // A previous editor may have closed with unread frames. Drain them before
    // re-enabling the producer so a reopened graph starts with current audio.
    supple::drive::gui::Frame stale;
    while (self->telemetryRing.pop(stale)) {
    }
    self->telemetryOn.store(true, std::memory_order_relaxed);
    if (!self->gui->create()) {
        self->telemetryOn.store(false, std::memory_order_relaxed);
        delete self->gui;
        self->gui = nullptr;
        return false;
    }
    startGuiTimer(self);
    return true;
}

void CLAP_ABI guiDestroy(const clap_plugin_t* p) {
    auto* self = (Plug*)p->plugin_data;
    self->telemetryOn.store(false, std::memory_order_relaxed);
    stopGuiTimer(self);
    delete self->gui;
    self->gui = nullptr;
}

bool CLAP_ABI guiSetScale(const clap_plugin_t* p, double scale) {
    auto* self = (Plug*)p->plugin_data;
#if defined(__APPLE__)
    // Cocoa is specified in logical pixels, so the host must not also scale us.
    (void)self;
    (void)scale;
    return false;
#else
    if (!self->gui) return false;
    self->gui->setScale(scale);
    return true;
#endif
}

bool CLAP_ABI guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto* self = (Plug*)p->plugin_data;
    if (!self->gui) return false;
    self->gui->getSize(*w, *h);
    return true;
}

bool CLAP_ABI guiCanResize(const clap_plugin_t*) { return true; }

bool CLAP_ABI guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    bool h = false, v = false, keep = false;
    uint32_t aw = 0, ah = 0;
    supple::drive::gui::Gui::resizeHints(h, v, keep, aw, ah);
    hints->can_resize_horizontally = h;
    hints->can_resize_vertically = v;
    hints->preserve_aspect_ratio = keep;
    hints->aspect_ratio_width = aw;
    hints->aspect_ratio_height = ah;
    return true;
}

bool CLAP_ABI guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto* self = (Plug*)p->plugin_data;
    if (!self->gui) return false;
    return self->gui->adjustSize(*w, *h);
}

bool CLAP_ABI guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    auto* self = (Plug*)p->plugin_data;
    return self->gui && self->gui->setSize(w, h);
}

bool CLAP_ABI guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    auto* self = (Plug*)p->plugin_data;
    if (!self->gui || !window) return false;
    // The union aliases all three handle types; pugl takes an integer either
    // way, so there is nothing per-platform to do here.
    const bool ok = self->gui->setParent((uintptr_t)window->ptr);
    // Not every wrapper implements clap.timer-support: clap-wrapper's AUv2
    // register_timer is a hard `return false`, which leaves the editor correct
    // but frozen -- input still arrives through the native view, so controls
    // work while nothing ever repaints. Drive it ourselves in that case.
    if (ok && !self->guiTimerRunning) self->gui->startFallbackTimer((double)kGuiTimerMs / 1000.0);
    return ok;
}

bool CLAP_ABI guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void CLAP_ABI guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool CLAP_ABI guiShow(const clap_plugin_t* p) {
    auto* self = (Plug*)p->plugin_data;
    if (!self->gui) return false;
    supple::drive::gui::Frame stale;
    while (self->telemetryRing.pop(stale)) {
    }
    const bool shown = self->gui->show();
    self->telemetryOn.store(shown, std::memory_order_relaxed);
    if (shown) {
        startGuiTimer(self);
        if (!self->guiTimerRunning) self->gui->startFallbackTimer((double)kGuiTimerMs / 1000.0);
    }
    return shown;
}

bool CLAP_ABI guiHide(const clap_plugin_t* p) {
    auto* self = (Plug*)p->plugin_data;
    self->telemetryOn.store(false, std::memory_order_relaxed);
    stopGuiTimer(self);
    if (self->gui) self->gui->stopFallbackTimer();
    return self->gui && self->gui->hide();
}

const clap_plugin_gui_t kGui = {
    guiIsApiSupported, guiGetPreferredApi, guiCreate,         guiDestroy,    guiSetScale,
    guiGetSize,        guiCanResize,       guiGetResizeHints, guiAdjustSize, guiSetSize,
    guiSetParent,      guiSetTransient,    guiSuggestTitle,   guiShow,       guiHide};

void CLAP_ABI plugOnGuiTimer(const clap_plugin_t* p, clap_id timerId) {
    auto* self = (Plug*)p->plugin_data;
    if (self->gui && timerId == self->guiTimerId) self->gui->tick();
}

const clap_plugin_timer_support_t kTimerSupport = {plugOnGuiTimer};

#endif  // DRIVE_WITH_GUI

const void* CLAP_ABI plugGetExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG)) return &kAudioPortsConfig;
    if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS)) return semanticExtension();
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
#if DRIVE_WITH_GUI
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
#endif
    return nullptr;
}

void CLAP_ABI plugOnMainThread(const clap_plugin_t* p) {
#if DRIVE_WITH_GUI
    auto* self = (Plug*)p->plugin_data;
    if (self->editRetryOnMain.exchange(false, std::memory_order_acq_rel) && self->hostParams &&
        self->hostParams->request_flush)
        self->hostParams->request_flush(self->host);
#else
    (void)p;
#endif
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* CLAP_ABI factoryGetDesc(const clap_plugin_factory_t*,
                                                        uint32_t index) {
    return index == 0 ? &kDesc : nullptr;
}

const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                            const char* id) {
    if (!id || std::strcmp(id, kDesc.id)) return nullptr;
    auto* self = new Plug();
    self->host = host;
    self->plugin.desc = &kDesc;
    self->plugin.plugin_data = self;
    self->plugin.init = plugInit;
    self->plugin.destroy = plugDestroy;
    self->plugin.activate = plugActivate;
    self->plugin.deactivate = plugDeactivate;
    self->plugin.start_processing = plugStartProcessing;
    self->plugin.stop_processing = plugStopProcessing;
    self->plugin.reset = plugReset;
    self->plugin.process = plugProcess;
    self->plugin.get_extension = plugGetExtension;
    self->plugin.on_main_thread = plugOnMainThread;
    return &self->plugin;
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryGetDesc, factoryCreate};

}  // namespace

// ---------------------------------------------------------------------------
// Entry points consumed by drive_entry.cpp (and by the VST3/AU wrappers).
// ---------------------------------------------------------------------------

bool drive_entry_init(const char*) { return true; }
void drive_entry_deinit() {}

const void* drive_entry_get_factory(const char* factory_id) {
    if (factory_id && !std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}
