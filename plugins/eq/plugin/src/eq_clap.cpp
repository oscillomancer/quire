// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Eq — CLAP plugin implementation.
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

#include <chrono>
#include <thread>
#include "EqParams.h"
#include "eq/processor.h"
#include "eq_entry.h"
#include "supple/clap/plugin_support.h"

#if EQ_WITH_GUI
#include "Gui.h"
#include "Telemetry.h"
#endif

namespace {

// The parameter table lives in EqParams.h so that the editor renders
// names, ranges and value text from the same source the host sees.
using namespace eq;

// ---------------------------------------------------------------------------

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                           CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDesc = {
    CLAP_VERSION_INIT,
    "com.suppleaudio.eq",
    supple::eq::kDisplayName,
    "Supple Audio",
    "",
    "",
    "",
    "0.1.0",
    "Eight-band parametric EQ with natural and linear phase modes.",
    kFeatures};

// ---------------------------------------------------------------------------

struct Plug
#if EQ_WITH_GUI
    : eq::gui::EditorHost
#endif
{
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;

    supple::eq::Processor processor;
    // Written by the audio thread (automation) and by the editor; read by
    // both. Plain doubles here would be a genuine race now that a GUI exists.
    supple::clapx::ParamStore values{kParams};
    supple::clapx::FlatSemantic semantic{supple::eq::patchType(), values, "com.suppleaudio.eq",
                                         "skill/guides/eq.md",    "",     0x51455131};
    supple::eq::Values dspValues{};
    std::uint64_t seen = 0;
    int activatedPhase = 1;
    int requestedPhase = -1;
    std::atomic<bool> workerOn{false};
    std::thread worker;
    Plug() {
        semantic.onApplied = [this](const auto&, const auto&) { checkRestart(); };
    }
    ~Plug() { stopWorker(); }
    void stopWorker() {
        workerOn.store(false, std::memory_order_release);
        if (worker.joinable()) worker.join();
    }
    void checkRestart() {
        const int phase = static_cast<int>(pv(supple::eq::kPhase));
        if (phase == activatedPhase) {
            requestedPhase = -1;
            return;
        }
        if (active && requestedPhase != phase && host && host->request_restart) {
            requestedPhase = phase;
            host->request_restart(host);
        }
    }
    void startWorker() {
        if (activatedPhase != 2) return;
        workerOn = true;
        const auto initialGeneration = seen;
        worker = std::thread([this, initialGeneration] {
            auto generation = initialGeneration;
            while (workerOn.load(std::memory_order_acquire)) {
                const auto next = values.generation();
                supple::eq::Values v{};
                if (next != generation && values.readAll(v)) {
                    v[supple::eq::kPhase] = 2;
                    processor.setLinearValues(v);
                    generation = next;
                }
                processor.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    std::atomic<double> srAtomic{48000.0};
    int channels = 2;
    int preparedChannels = 0;
    bool active = false;
    std::atomic<int> latencyAtomic{0};
    // Scratch for process(), sized once from the selected mono/stereo port
    // configuration during activation and never resized on the audio thread.
    std::vector<float*> ptrs32;
    std::vector<double*> ptrs64;

    void sizeScratch(int nch) {
        ptrs32.resize((size_t)nch);
        ptrs64.resize((size_t)nch);
    }

    double pv(uint32_t id) const { return values.get(id); }
    void sv(uint32_t id, double v) { values.set(id, v); }
    void loadDefaults() { values.resetToDefaults(); }
    void pushParams() {
        const auto g = values.generation();
        if (g == seen) return;
        if (values.readAll(dspValues)) {
            for (std::size_t i = 0; i < dspValues.size(); ++i)
                if (i != supple::eq::kPhase || !active) processor.setParam(i, dspValues[i]);
            seen = g;
        }
    }
#if EQ_WITH_GUI
    eq::gui::TelemetryRing telemetryRing;
    eq::gui::ParamRing editRing;
    eq::gui::GestureRing deferredGestureRing;
    eq::gui::Gui* gui = nullptr;
    // Whether anything is looking. Read by the audio thread every chunk, so it
    // is atomic; written by the main thread from the editor's lifecycle.
    std::atomic<bool> telemetryOn{false};
    bool guiTimerRunning = false;
    clap_id guiTimerId = 0;
    int telemetryChunk = 192;
    // Main-thread gesture producer state.
    std::array<bool, kNumParams> gestureQueued{};
    std::size_t reserved = 0;
    // Audio-thread retry state for one compact deferred gesture.
    bool deferredGestureActive = false;
    eq::gui::ParamGesture deferredGesture{};
    int deferredGesturePhase = 0;
    std::atomic<bool> editRetryOnMain{false};

    eq::gui::Frame spectrumFrame{};
    std::size_t spectrumFill = 0;
    template <class T>
    void runChunks(std::vector<T*>& v, int n) {
        pushParams();
        const bool capture = telemetryOn.load(std::memory_order_relaxed);
        for (int offset = 0; offset < n;) {
            const int count =
                capture ? std::min(n - offset, 256 - static_cast<int>(spectrumFill)) : n - offset;
            const T* input[2] = {v[0] + offset, v[static_cast<std::size_t>(channels - 1)] + offset};
            T* output[2] = {v[0] + offset, v[static_cast<std::size_t>(channels - 1)] + offset};
            if (capture)
                for (int f = 0; f < count; ++f) {
                    spectrumFrame.input[spectrumFill + static_cast<std::size_t>(f)] =
                        static_cast<float>(input[0][f]);
                    spectrumFrame.inputRight[spectrumFill + static_cast<std::size_t>(f)] =
                        static_cast<float>(input[1][f]);
                }
            processor.process(input, output, static_cast<uint32_t>(count));
            if (capture) {
                for (int f = 0; f < count; ++f) {
                    spectrumFrame.output[spectrumFill + static_cast<std::size_t>(f)] =
                        static_cast<float>(output[0][f]);
                    spectrumFrame.outputRight[spectrumFill + static_cast<std::size_t>(f)] =
                        static_cast<float>(output[1][f]);
                }
                spectrumFill += static_cast<std::size_t>(count);
                if (spectrumFill == 256) {
                    telemetryRing.push(spectrumFrame);
                    spectrumFill = 0;
                }
            }
            offset += count;
        }
    }
#else
    template <class T>
    void runChunks(std::vector<T*>& v, int n) {
        pushParams();
        const T* input[2] = {v[0], v[static_cast<std::size_t>(channels - 1)]};
        processor.process(input, v.data(), static_cast<uint32_t>(n));
    }
#endif

#if EQ_WITH_GUI

    // --- EditorHost ---------------------------------------------------------

    double paramValue(uint32_t id) const override { return pv(id); }

    void beginEdit(uint32_t id) override {
        gestureQueued[id] = !deferredGestureRing.pending() && editRing.free() >= reserved + 3 &&
                            queueEdit({id, supple::params::toClap(*findParam(id), pv(id)),
                                       eq::gui::ParamEdit::kBegin},
                                      true);
        if (gestureQueued[id]) reserved += 2;
        if (!gestureQueued[id] && hostParams && hostParams->request_flush)
            hostParams->request_flush(host);
    }

    void setParam(uint32_t id, double v) override {
        const ParamDef* d = findParam(id);
        if (!d || (d->flags & supple::params::kReadonly)) return;
        const double c = std::max(d->min, std::min(d->max, v));
        // Apply locally at once so the knob tracks the mouse even with the
        // transport stopped; the ring is what tells the host.
        sv(id, c);
        checkRestart();
        if (gestureQueued[id])
            queueEdit({id, supple::params::toClap(*d, c), eq::gui::ParamEdit::kValue});
    }

    void endEdit(uint32_t id) override {
        if (gestureQueued[id]) {
            // The begin reserved these two slots for the exact final value/end.
            queueEdit(
                {id, supple::params::toClap(*findParam(id), pv(id)), eq::gui::ParamEdit::kValue},
                true);
            queueEdit({id, pv(id), eq::gui::ParamEdit::kEnd}, true);
        } else {
            deferredGestureRing.push({id, supple::params::toClap(*findParam(id), pv(id))});
            if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
        }
        if (gestureQueued[id]) reserved -= 2;
        gestureQueued[id] = false;
    }

    eq::gui::TelemetryRing& telemetry() override { return telemetryRing; }

    int latencySamples() const override { return latencyAtomic.load(std::memory_order_relaxed); }

    double sampleRate() const override { return srAtomic.load(std::memory_order_relaxed); }

    bool queueEdit(const eq::gui::ParamEdit& e, bool boundary = false) {
        if (!boundary && e.kind == eq::gui::ParamEdit::kValue && editRing.free() <= reserved)
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
    return (uint32_t)((Plug*)p->plugin_data)->latencyAtomic.load(std::memory_order_relaxed);
}
const clap_plugin_latency_t kLatency = {latGet};

// ---------------------------------------------------------------------------
// Extension: params
// ---------------------------------------------------------------------------

uint32_t CLAP_ABI paramCount(const clap_plugin_t*) { return static_cast<uint32_t>(kParamCount); }
bool CLAP_ABI paramInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    return supple::clapx::ClapParams::info(kParams, index, info);
}
bool CLAP_ABI paramGetValue(const clap_plugin_t* p, clap_id id, double* out) {
    return static_cast<Plug*>(p->plugin_data)->values.getClap(id, *out);
}
bool CLAP_ABI paramValueToText(const clap_plugin_t*, clap_id id, double v, char* out,
                               uint32_t size) {
    return supple::clapx::ClapParams::valueToText(kParams, id, v, out, size);
}
bool CLAP_ABI paramTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out) {
    return supple::clapx::ClapParams::textToValue(kParams, id, text, out);
}
void applyEvent(Plug* self, const clap_event_header_t* h) {
    if (h && h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE &&
        h->size >= sizeof(clap_event_param_value_t)) {
        const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
        self->values.setClap(ev->param_id, ev->value);
        if (ev->param_id == supple::eq::kPhase && self->host && self->host->request_callback)
            self->host->request_callback(self->host);
    }
}

// Edits made in the editor reach the host from here and from process(). Both
// are required: process() covers playback, paramFlush() covers a stopped
// transport, and the host decides which one it calls.
// Returns true if any edit was drained, which means values[] has moved and the
// processor has to be told. Forgetting that is silent and total: the knob tracks
// the mouse, the host records the automation, and the audio never changes.
#if EQ_WITH_GUI
bool emitEdit(const clap_output_events_t* out, const eq::gui::ParamEdit& e, uint32_t frame) {
    if (!out) return false;
    if (e.kind == eq::gui::ParamEdit::kValue) {
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
        ev.value = e.value;
        return out->try_push(out, &ev.header);
    }
    clap_event_param_gesture_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.time = frame;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = e.kind == eq::gui::ParamEdit::kBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
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
#if EQ_WITH_GUI
    bool any = false;
    eq::gui::ParamEdit e;
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
        const auto kind = self->deferredGesturePhase == 0   ? eq::gui::ParamEdit::kBegin
                          : self->deferredGesturePhase == 1 ? eq::gui::ParamEdit::kValue
                                                            : eq::gui::ParamEdit::kEnd;
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
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) applyEvent(self, in->get(in, i));
    drainEdits(self, out, 0);
    if (self->host && self->host->request_callback) self->host->request_callback(self->host);
}

const clap_plugin_params_t kParamsExt = {paramCount,       paramInfo,        paramGetValue,
                                         paramValueToText, paramTextToValue, paramFlush};

// ---------------------------------------------------------------------------
// Extension: state
// ---------------------------------------------------------------------------
//
// Versioned id/value pairs rather than a raw struct dump, so that reordering or
// adding parameters later cannot silently misread an old session.

bool CLAP_ABI stateSave(const clap_plugin_t* p, const clap_ostream_t* out) {
    return static_cast<Plug*>(p->plugin_data)->semantic.saveState(out);
}
bool CLAP_ABI stateLoad(const clap_plugin_t* p, const clap_istream_t* in) {
    auto* self = static_cast<Plug*>(p->plugin_data);
    if (!self->semantic.loadState(in)) return false;
    if (self->hostParams && self->hostParams->rescan)
        self->hostParams->rescan(self->host, CLAP_PARAM_RESCAN_VALUES);
    return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};
supple::clapx::FlatSemantic* resolveSemantic(const clap_plugin_t* p) {
    return &static_cast<Plug*>(p->plugin_data)->semantic;
}
bool CLAP_ABI describe(const clap_plugin_t* p, uint32_t detail, const clap_ostream_t* out) {
    const auto patch = static_cast<Plug*>(p->plugin_data)->semantic.current();
    supple::eq::Values v{};
    std::copy(patch.body.values.begin(), patch.body.values.end(), v.begin());
    const auto text = supple::eq::describe(v, detail == CLAP_SUPPLE_PATCH_DETAIL_FULL
                                                  ? supple::patch::Detail::Full
                                                  : supple::patch::Detail::Changed);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto n = out->write(out, text.data() + pos, text.size() - pos);
        if (n <= 0 || static_cast<std::size_t>(n) > text.size() - pos) return false;
        pos += static_cast<std::size_t>(n);
    }
    return true;
}
const auto kSemantic = [] {
    auto ext = *supple::clapx::FlatSemantic::extension<resolveSemantic>();
    ext.describe = describe;
    return ext;
}();

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

#if EQ_WITH_GUI
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
#if EQ_WITH_GUI
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
#if EQ_WITH_GUI
    // 250 Hz of telemetry at any session rate.
    self->telemetryChunk = std::max(1, (int)std::lround(sr / 250.0));
#endif
    self->active = false;
    self->seen = 0;
    self->pushParams();
    self->activatedPhase = static_cast<int>(self->pv(supple::eq::kPhase));
    self->processor.prepare(sr, maxFrames, self->channels);
    self->latencyAtomic.store(self->processor.latency(), std::memory_order_relaxed);
    if (self->host) {
        const auto* h = static_cast<const clap_host_latency_t*>(
            self->host->get_extension(self->host, CLAP_EXT_LATENCY));
        if (h && h->changed) h->changed(self->host);
    }
    self->preparedChannels = self->channels;
    self->sizeScratch(self->channels);
    self->pushParams();
    self->requestedPhase = -1;
    self->active = true;
    self->startWorker();
    return true;
}

void CLAP_ABI plugDeactivate(const clap_plugin_t* p) {
    auto* self = static_cast<Plug*>(p->plugin_data);
    self->stopWorker();
    self->active = false;
}

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
            self->runChunks(self->ptrs32, (int)n);
        } else if (ob.data64 && ib.data64) {
            for (uint32_t c = 0; c < nch; ++c) {
                if (ob.data64[c] != ib.data64[c])
                    std::memcpy(ob.data64[c] + frame, ib.data64[c] + frame, n * sizeof(double));
                self->ptrs64[c] = ob.data64[c] + frame;
            }
            self->runChunks(self->ptrs64, (int)n);
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
#if EQ_WITH_GUI

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
    if (!self->gui) self->gui = new eq::gui::Gui(*self);
    // A previous editor may have closed with unread frames. Drain them before
    // re-enabling the producer so a reopened graph starts with current audio.
    eq::gui::Frame stale;
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
    eq::gui::Gui::resizeHints(h, v, keep, aw, ah);
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
    eq::gui::Frame stale;
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

#endif  // EQ_WITH_GUI

const void* CLAP_ABI plugGetExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG)) return &kAudioPortsConfig;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS)) return &kSemantic;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
#if EQ_WITH_GUI
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
#endif
    return nullptr;
}

void CLAP_ABI plugOnMainThread(const clap_plugin_t* p) {
    static_cast<Plug*>(p->plugin_data)->checkRestart();
#if EQ_WITH_GUI
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
// Entry points consumed by eq_entry.cpp (and by the VST3/AU wrappers).
// ---------------------------------------------------------------------------

bool eq_entry_init(const char*) { return true; }
void eq_entry_deinit() {}

const void* eq_entry_get_factory(const char* factory_id) {
    if (factory_id && !std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}
