// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Shared black-box CLAP contract checks. Keep identical across products.
#pragma once

#include <clap/clap.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <limits>
#include <type_traits>

namespace host_contract {
using Check = void (*)(bool, const char*);

// Config IDs are opaque: Ferric's stereo ID is 0, not the template's 2.
// A plugin without the configuration extension can use its current layout.
inline bool selectLayout(const clap_plugin_t* plug, uint32_t channels) {
    const auto* configs = static_cast<const clap_plugin_audio_ports_config_t*>(
        plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS_CONFIG));
    if (configs) {
        for (uint32_t i = 0; i < configs->count(plug); ++i) {
            clap_audio_ports_config_t c{};
            if (configs->get(plug, i, &c) && c.has_main_output &&
                c.main_output_channel_count == channels &&
                (!c.has_main_input || c.main_input_channel_count == channels))
                return configs->select(plug, c.id);
        }
        return false;
    }
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS));
    clap_audio_port_info_t out{}, in{};
    return ports && ports->get(plug, 0, false, &out) && out.channel_count == channels &&
        (ports->count(plug, true) == 0 ||
         (ports->get(plug, 0, true, &in) && in.channel_count == channels));
}

inline void ports(const clap_plugin_t* plug, uint32_t channels, Check check) {
    const auto* ext = static_cast<const clap_plugin_audio_ports_t*>(
        plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS));
    check(ext != nullptr, "audio port extension available");
    if (!ext) return;
    const bool instrument = ext->count(plug, true) == 0;
    clap_audio_port_info_t out{}, in{};
    const bool haveOut = ext->count(plug, false) == 1 && ext->get(plug, 0, false, &out);
    check(haveOut, "reports one output port");
    if (!haveOut) return;
    check((out.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0, "output is the main bus");
    check(out.channel_count == channels, "output matches the negotiated channel count");
    const char* type = channels == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    check(out.port_type && !std::strcmp(out.port_type, type), "output declares the negotiated port type");
    check((out.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) != 0, "output advertises 64-bit audio");
    if (instrument) {
        check(out.in_place_pair == CLAP_INVALID_ID, "instrument does not pair with a nonexistent input");
        return;
    }
    const bool haveIn = ext->count(plug, true) == 1 && ext->get(plug, 0, true, &in);
    check(haveIn, "effect reports one input port");
    if (!haveIn) return;
    check((in.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0, "input is the main bus");
    check(in.channel_count == channels && in.port_type && !std::strcmp(in.port_type, type),
          "input layout agrees with output");
    check((in.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) != 0, "input advertises 64-bit audio");
    check((in.flags & CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE) != 0 &&
          (out.flags & CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE) != 0,
          "effect ports require matching sample precision");
    check(in.in_place_pair != CLAP_INVALID_ID && out.in_place_pair != CLAP_INVALID_ID &&
          in.in_place_pair == out.id && out.in_place_pair == in.id,
          "effect in-place pairing is reciprocal");
}
inline void parameterText(const clap_plugin_t* plug, Check check) {
    const auto* params = static_cast<const clap_plugin_params_t*>(
        plug->get_extension(plug, CLAP_EXT_PARAMS));
    check(params != nullptr, "parameter extension available for text checks");
    if (!params) return;
    bool roundTrip = true, rejectsNonfinite = true;
    size_t cases = 0;
    for (uint32_t i = 0; i < params->count(plug); ++i) {
        clap_param_info_t info{};
        if (!params->get_info(plug, i, &info)) {
            roundTrip = rejectsNonfinite = false;
            continue;
        }
        const bool stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        const double range = info.max_value - info.min_value;
        std::vector<double> values{info.min_value, info.default_value, info.max_value};
        if (stepped && range <= 256.0) {
            for (double v = info.min_value; v <= info.max_value; v += 1.0)
                values.push_back(v);
        } else {
            for (double fraction : {0.25, 0.5, 0.75}) {
                const double v = info.min_value + fraction * range;
                values.push_back(stepped ? std::round(v) : v);
            }
        }
        for (double v : values) {
            char text[256]{}, again[256]{};
            double decoded = 0.0;
            // Continuous displays are lossy (whole Hz, percentages, etc.).
            // Their round-trip contract is stable display text and a finite,
            // in-range value, not an arbitrary range-based numeric tolerance.
            // Stepped values, including every small enum, must be exact.
            const bool ok = params->value_to_text(plug, info.id, v, text, sizeof(text)) &&
                params->text_to_value(plug, info.id, text, &decoded) &&
                std::isfinite(decoded) && decoded >= info.min_value && decoded <= info.max_value &&
                (!stepped || decoded == v) &&
                params->value_to_text(plug, info.id, decoded, again, sizeof(again)) &&
                !std::strcmp(text, again);
            if (!ok) {
                std::printf("     text round-trip: id=%u %s value=%g text=\"%s\" decoded=%g\n",
                            info.id, info.name, v, text, decoded);
                roundTrip = false;
            }
            ++cases;
        }
        // Rejection, not merely a finite/clamped result, is the contract.
        for (const char* text : {"", " ", "garbage", "nan", "-nan", "NaN", "nan(1)", "nan dB", "inf",
                                 "-inf", "Inf", "infinity", "INFINITY", "1e999", "-1e999"}) {
            double decoded = 12345.0;
            if (params->text_to_value(plug, info.id, text, &decoded)) {
                std::printf("     accepted invalid text: id=%u %s text=\"%s\" decoded=%g\n",
                            info.id, info.name, text, decoded);
                rejectsNonfinite = false;
            }
        }
    }
    check(roundTrip, "parameter labels round-trip at defaults, bounds and interior values");
    check(rejectsNonfinite, "all parameters reject invalid, non-finite and overflowing text");
    std::printf("     parameter round-trip cases: %zu\n", cases);
}

// A separate host context per instance keeps these renders independent of the
// smoke test's restart counters. Callbacks are serviced after process(), never
// reentrantly from the audio callback.
struct RenderInstance {
    const clap_plugin_t* plug = nullptr;
    clap_host_t host{};
    bool active = false, processing = false, pending = false;
    static const void* CLAP_ABI extension(const clap_host_t*, const char*) { return nullptr; }
    static void CLAP_ABI ignored(const clap_host_t*) {}
    static void CLAP_ABI callback(const clap_host_t* h) {
        static_cast<RenderInstance*>(h->host_data)->pending = true;
    }
    RenderInstance(const clap_plugin_factory_t* factory, const char* id) {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "contract-render";
        host.vendor = "test";
        host.url = "";
        host.version = "1";
        host.get_extension = extension;
        host.request_restart = ignored;
        host.request_process = ignored;
        host.request_callback = callback;
        plug = factory->create_plugin(factory, &host, id);
        if (plug && !plug->init(plug)) { plug->destroy(plug); plug = nullptr; }
    }
    RenderInstance(const RenderInstance&) = delete;
    RenderInstance& operator=(const RenderInstance&) = delete;
    void pump() {
        if (pending) { pending = false; plug->on_main_thread(plug); }
    }
    ~RenderInstance() {
        if (!plug) return;
        if (processing) plug->stop_processing(plug);
        if (active) plug->deactivate(plug);
        plug->destroy(plug);
    }
};

struct InputEvents {
    std::vector<const clap_event_header_t*> events;
    static uint32_t CLAP_ABI count(const clap_input_events_t* list) {
        return static_cast<uint32_t>(static_cast<const InputEvents*>(list->ctx)->events.size());
    }
    static const clap_event_header_t* CLAP_ABI get(const clap_input_events_t* list, uint32_t i) {
        const auto& events = static_cast<const InputEvents*>(list->ctx)->events;
        return i < events.size() ? events[i] : nullptr;
    }
    static bool CLAP_ABI push(const clap_output_events_t*, const clap_event_header_t*) { return true; }
    clap_input_events_t input{this, count, get};
    clap_output_events_t output{nullptr, push};
};

struct Render {
    std::vector<double> samples;
    bool ok = false;
    double peak = 0.0, inputDifference = 0.0;
};

template<class Sample>
Render render(const clap_plugin_factory_t* factory, const char* id, const char* plainParam,
              uint32_t channels, double sampleRate, bool inPlace, bool instrument) {
    Render result;
    RenderInstance instance(factory, id);
    const auto* plug = instance.plug;
    if (!plug || !selectLayout(plug, channels)) return result;
    const auto* params = static_cast<const clap_plugin_params_t*>(
        plug->get_extension(plug, CLAP_EXT_PARAMS));
    if (!params) return result;
    InputEvents events;
    bool configured = false;
    // Non-unity gain/drive catches a path which just returns the input. Pin
    // every instrument seed so two fresh instances receive the same randomness.
    for (uint32_t i = 0; i < params->count(plug); ++i) {
        clap_param_info_t info{};
        if (!params->get_info(plug, i, &info)) return result;
        if (info.flags & CLAP_PARAM_IS_READONLY) continue;
        const bool plain = !std::strcmp(info.name, plainParam);
        const bool seed = instrument && !std::strcmp(info.name, "Seed");
        if (!plain && !seed) continue;
        clap_event_param_value_t value{};
        value.header.size = sizeof(value);
        value.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        value.header.type = CLAP_EVENT_PARAM_VALUE;
        value.param_id = info.id;
        value.note_id = -1;
        value.port_index = value.channel = value.key = -1;
        value.value = std::clamp(seed ? 42.0 : info.default_value +
            0.125 * (info.max_value - info.min_value), info.min_value, info.max_value);
        events.events = {&value.header};
        params->flush(plug, &events.input, &events.output);
        double back = 0.0;
        if (!params->get_value(plug, info.id, &back) || back != value.value) return result;
        configured |= plain;
        instance.pump();
    }
    events.events.clear();
    if (!configured || !plug->activate(plug, sampleRate, 1, 512)) return result;
    instance.active = true;
    if (!plug->start_processing(plug)) return result;
    instance.processing = true;

    // Each channel owns distinct storage. In-place aliases input/output only
    // within that channel. Different L/R signals expose accidental cross-talk.
    std::vector<std::vector<Sample>> input(channels, std::vector<Sample>(512));
    std::vector<std::vector<Sample>> output(channels, std::vector<Sample>(512));
    std::vector<Sample*> in(channels), out(channels);
    for (uint32_t c = 0; c < channels; ++c) {
        in[c] = input[c].data();
        out[c] = inPlace ? in[c] : output[c].data();
    }
    clap_audio_buffer_t ib{}, ob{};
    ib.channel_count = ob.channel_count = channels;
    if constexpr (std::is_same_v<Sample, float>) { ib.data32 = in.data(); ob.data32 = out.data(); }
    else { ib.data64 = in.data(); ob.data64 = out.data(); }
    clap_process_t proc{};
    proc.audio_inputs = instrument ? nullptr : &ib;
    proc.audio_inputs_count = instrument ? 0u : 1u;
    proc.audio_outputs = &ob;
    proc.audio_outputs_count = 1;
    proc.in_events = &events.input;
    proc.out_events = &events.output;
    constexpr uint32_t blocks[] = {1, 64, 257, 512};
    uint64_t frame = 0;
    result.ok = true;
    for (int b = 0; b < 32; ++b) {
        const uint32_t count = blocks[b % 4];
        proc.frames_count = count;
        proc.steady_time = static_cast<int64_t>(frame);
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint32_t i = 0; i < count; ++i) {
                // Quantize both paths' input to float, then widen for double.
                // Otherwise this test would include input quantization error.
                const double t = static_cast<double>(frame + i) / sampleRate;
                const float v = static_cast<float>((c == 0 ? 0.23 : 0.13) *
                    std::sin(6.283185307179586 * (c == 0 ? 220.0 : 371.0) * t + c));
                input[c][i] = static_cast<Sample>(v);
                if (!inPlace) output[c][i] = std::numeric_limits<Sample>::quiet_NaN();
            }
        }
        clap_event_note_t note{};
        events.events.clear();
        if (instrument && (b == 1 || b == 5 || b == 13 || b == 17)) {
            note.header.size = sizeof(note);
            note.header.time = 7;
            note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            note.header.type = (b == 1 || b == 13) ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
            note.note_id = b < 13 ? 1 : 2;
            note.port_index = 0;
            note.channel = 0;
            note.key = 36;
            note.velocity = 0.8;
            events.events = {&note.header};
        }
        if (plug->process(plug, &proc) == CLAP_PROCESS_ERROR) result.ok = false;
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint32_t i = 0; i < count; ++i) {
                const double v = out[c][i];
                if (!std::isfinite(v)) result.ok = false;
                result.samples.push_back(v);
                result.peak = std::max(result.peak, std::fabs(v));
                if (!inPlace && !instrument)
                    result.inputDifference = std::max(result.inputDifference,
                        std::fabs(v - static_cast<double>(input[c][i])));
            }
        }
        frame += count;
        instance.pump();
    }
    return result;
}

inline double difference(const Render& a, const Render& b) {
    if (!a.ok || !b.ok || a.samples.empty() || a.samples.size() != b.samples.size())
        return std::numeric_limits<double>::infinity();
    double error = 0.0;
    for (size_t i = 0; i < a.samples.size(); ++i)
        error = std::max(error, std::fabs(a.samples[i] - b.samples[i]));
    return error;
}

inline void processing(const clap_plugin_factory_t* factory, const char* id,
                       const char* plainParam, uint32_t channels, double sampleRate,
                       bool instrument, Check check) {
    // Each render starts with a fresh, identically configured instance. Comparing
    // successive calls on one live processor compares different DSP histories.
    const auto f = render<float>(factory, id, plainParam, channels, sampleRate, false, instrument);
    const auto d = render<double>(factory, id, plainParam, channels, sampleRate, false, instrument);
    check(f.ok && f.peak > 1e-5, "float render writes finite, non-silent audio");
    check(d.ok && d.peak > 1e-5, "double render writes finite, non-silent audio");
    const double precisionError = difference(f, d);
    // Absolute sample tolerance, with exactly representable shared inputs.
    check(precisionError < 1e-5, "float/double full renders agree from identical initial state");
    std::printf("     float/double max error: %.9g; peaks: %.6g / %.6g\n",
                precisionError, f.peak, d.peak);
    if (instrument) return; // No input port means no in-place promise.
    check(f.inputDifference > 1e-4 && d.inputDifference > 1e-4,
          "both effect precision paths reach non-identity DSP");
    const auto fi = render<float>(factory, id, plainParam, channels, sampleRate, true, false);
    const auto di = render<double>(factory, id, plainParam, channels, sampleRate, true, false);
    const double floatError = difference(f, fi), doubleError = difference(d, di);
    check(floatError == 0.0, "float in-place matches separate buffers on every channel");
    check(doubleError == 0.0, "double in-place matches separate buffers on every channel");
    std::printf("     in-place errors float/double: %.9g / %.9g\n", floatError, doubleError);
}
} // namespace host_contract
