// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Shared black-box CLAP contract checks. Keep identical across products.
#pragma once

#include <clap/clap.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <limits>
#include <type_traits>

namespace host_contract {
using Check = void (*)(bool, const char*);

// A main bus layout as product.cmake spells it: 1 2 5.1 7.1 7.1.4 ambi1..3,
// a channel count, or "none" for a plugin with no audio. What follows is
// what each must look like to a host -- stated here independently of the
// framework's own tables.
struct LayoutSpec {
    uint32_t channels = 2;
    const char* portType = CLAP_PORT_STEREO;  // null: no type
    std::vector<uint8_t> speakers;            // surround only
    bool ambisonic = false;
};

inline bool parseLayout(const char* name, LayoutSpec& out) {
    out = LayoutSpec{};
    const std::string n = name ? name : "";
    if (n == "none") { out.channels = 0; out.portType = nullptr; return true; }
    if (n == "1") { out.channels = 1; out.portType = CLAP_PORT_MONO; return true; }
    if (n == "2") return true;
    if (n == "5.1" || n == "7.1" || n == "7.1.4") {
        out.portType = CLAP_PORT_SURROUND;
        out.speakers = {CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC, CLAP_SURROUND_LFE,
                        CLAP_SURROUND_BL, CLAP_SURROUND_BR};
        if (n != "5.1") out.speakers.insert(out.speakers.end(), {CLAP_SURROUND_SL, CLAP_SURROUND_SR});
        if (n == "7.1.4")
            out.speakers.insert(out.speakers.end(),
                                {CLAP_SURROUND_TFL, CLAP_SURROUND_TFR, CLAP_SURROUND_TBL, CLAP_SURROUND_TBR});
        out.channels = static_cast<uint32_t>(out.speakers.size());
        return true;
    }
    if (n.size() == 5 && n.compare(0, 4, "ambi") == 0 && n[4] >= '1' && n[4] <= '3') {
        const uint32_t order = static_cast<uint32_t>(n[4] - '0');
        out.channels = (order + 1) * (order + 1);
        out.portType = CLAP_PORT_AMBISONIC;
        out.ambisonic = true;
        return true;
    }
    char* end = nullptr;
    const long count = std::strtol(n.c_str(), &end, 10);
    if (n.empty() || *end || count < 3 || count > 64) return false;
    out.channels = static_cast<uint32_t>(count);
    out.portType = nullptr;
    return true;
}

inline const clap_plugin_audio_ports_t* audioPorts(const clap_plugin_t* plug) {
    return static_cast<const clap_plugin_audio_ports_t*>(plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS));
}

// Whether the plugin has a main audio input (an effect) -- the one fact
// about its kind the checks below need.
inline bool hasMainInput(const clap_plugin_t* plug) {
    const auto* ports = audioPorts(plug);
    for (uint32_t i = 0; ports && i < ports->count(plug, true); ++i) {
        clap_audio_port_info_t info{};
        if (ports->get(plug, i, true, &info) && (info.flags & CLAP_AUDIO_PORT_IS_MAIN)) return true;
    }
    return false;
}

// Config IDs are opaque: this framework uses the channel count, other
// plugins use anything. Search by layout, never by ID.
// A plugin without the configuration extension can use its current layout.
inline bool selectLayout(const clap_plugin_t* plug, uint32_t channels) {
    const auto* ports = audioPorts(plug);
    if (channels == 0) return !ports || ports->count(plug, false) == 0;
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
    clap_audio_port_info_t out{};
    return ports && ports->get(plug, 0, false, &out) && out.channel_count == channels;
}

inline void ports(const clap_plugin_t* plug, const LayoutSpec& layout, Check check) {
    const auto* ext = audioPorts(plug);
    check(ext != nullptr, "audio port extension available");
    if (!ext) return;
    if (layout.channels == 0) {
        check(ext->count(plug, true) == 0 && ext->count(plug, false) == 0, "a note effect has no audio ports");
        return;
    }
    bool shapes = true, types = true, flags = true, ids = true, pairing = true;
    int mainIn = -1, mainOut = -1;
    clap_id mainInId = CLAP_INVALID_ID, mainOutId = CLAP_INVALID_ID;
    for (int dir = 0; dir < 2; ++dir) {
        const bool input = dir == 0;
        std::vector<clap_id> seen;
        for (uint32_t i = 0; i < ext->count(plug, input); ++i) {
            clap_audio_port_info_t p{};
            if (!ext->get(plug, i, input, &p)) { shapes = false; continue; }
            const bool main = (p.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;
            if (main) {
                if ((input ? mainIn : mainOut) >= 0) shapes = false;  // one main bus each way
                (input ? mainIn : mainOut) = static_cast<int>(i);
                (input ? mainInId : mainOutId) = p.id;
                if (i != 0) shapes = false;  // and it comes first
            }
            // Every bus here is as wide as the layout: a sidechain, an aux output.
            shapes = shapes && p.channel_count == layout.channels && p.name[0] != '\0';
            types = types && ((!layout.portType && !p.port_type) ||
                              (layout.portType && p.port_type && !std::strcmp(p.port_type, layout.portType)));
            flags = flags && (p.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) &&
                    (p.flags & CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE);
            ids = ids && std::find(seen.begin(), seen.end(), p.id) == seen.end();
            seen.push_back(p.id);
            if (!main && p.in_place_pair != CLAP_INVALID_ID) pairing = false;
        }
    }
    check(mainOut == 0, "the main output is the first output port");
    check(shapes, "one main bus each way, every port as wide as the layout and named");
    check(types, "every port declares the layout's port type");
    check(flags, "every port advertises 64-bit audio and one precision throughout");
    check(ids, "port IDs are unique in each direction");
    if (mainIn >= 0) {
        clap_audio_port_info_t in{}, out{};
        ext->get(plug, 0, true, &in);
        ext->get(plug, 0, false, &out);
        pairing = pairing && in.in_place_pair == mainOutId && out.in_place_pair == mainInId;
    } else {
        clap_audio_port_info_t out{};
        ext->get(plug, 0, false, &out);
        pairing = pairing && out.in_place_pair == CLAP_INVALID_ID;
    }
    check(pairing, "only the main buses pair in place, reciprocally");

    if (!layout.speakers.empty()) {
        const auto* surround = static_cast<const clap_plugin_surround_t*>(plug->get_extension(plug, CLAP_EXT_SURROUND));
        check(surround != nullptr, "surround extension available");
        if (!surround) return;
        uint64_t mask = 0;
        for (uint8_t sp : layout.speakers) mask |= uint64_t{1} << sp;
        check(surround->is_channel_mask_supported(plug, mask), "the layout's channel mask is supported");
        bool maps = true;
        for (int dir = 0; dir < 2; ++dir)
            for (uint32_t i = 0; i < ext->count(plug, dir == 0); ++i) {
                uint8_t map[64]{};
                const uint32_t n = surround->get_channel_map(plug, dir == 0, i, map, 64);
                maps = maps && n == layout.channels && std::equal(layout.speakers.begin(), layout.speakers.end(), map);
            }
        check(maps, "every port's channel map is the layout's speakers in order");
    }
    if (layout.ambisonic) {
        const auto* ambi = static_cast<const clap_plugin_ambisonic_t*>(plug->get_extension(plug, CLAP_EXT_AMBISONIC));
        check(ambi != nullptr, "ambisonic extension available");
        if (!ambi) return;
        bool configs = true;
        for (int dir = 0; dir < 2; ++dir)
            for (uint32_t i = 0; i < ext->count(plug, dir == 0); ++i) {
                clap_ambisonic_config_t c{};
                configs = configs && ambi->get_config(plug, dir == 0, i, &c) &&
                          c.ordering == CLAP_AMBISONIC_ORDERING_ACN && c.normalization == CLAP_AMBISONIC_NORMALIZATION_SN3D;
            }
        check(configs, "every port is ACN/SN3D ambisonics");
    }
}

// Buffers for every audio port the plugin reports, each its own storage; the
// main output may alias the main input (in place). Sidechains carry their
// own signal, so a plugin that keys from one is exercised.
template <class Sample>
struct Buses {
    std::vector<std::vector<std::vector<Sample>>> in, out;  // [port][channel][frame]
    std::vector<std::vector<Sample*>> inPointers, outPointers;
    std::vector<clap_audio_buffer_t> inBuffers, outBuffers;
    int mainIn = -1;
    uint32_t frames = 0;

    void build(const clap_plugin_t* plug, uint32_t capacity, bool inPlace) {
        const auto* ports = audioPorts(plug);
        frames = capacity;
        for (int dir = 0; dir < 2; ++dir) {
            const bool input = dir == 0;
            auto& store = input ? in : out;
            auto& pointers = input ? inPointers : outPointers;
            auto& buffers = input ? inBuffers : outBuffers;
            const uint32_t count = ports ? ports->count(plug, input) : 0;
            store.assign(count, {});
            pointers.assign(count, {});
            buffers.assign(count, clap_audio_buffer_t{});
            for (uint32_t i = 0; i < count; ++i) {
                clap_audio_port_info_t p{};
                ports->get(plug, i, input, &p);
                if (input && (p.flags & CLAP_AUDIO_PORT_IS_MAIN)) mainIn = static_cast<int>(i);
                store[i].assign(p.channel_count, std::vector<Sample>(capacity, Sample(0)));
                for (auto& c : store[i]) pointers[i].push_back(c.data());
                buffers[i].channel_count = p.channel_count;
            }
        }
        if (inPlace && mainIn >= 0 && !out.empty())
            for (size_t c = 0; c < outPointers[0].size() && c < inPointers[static_cast<size_t>(mainIn)].size(); ++c)
                outPointers[0][c] = inPointers[static_cast<size_t>(mainIn)][c];
        for (int dir = 0; dir < 2; ++dir) {
            auto& pointers = dir == 0 ? inPointers : outPointers;
            auto& buffers = dir == 0 ? inBuffers : outBuffers;
            for (size_t i = 0; i < buffers.size(); ++i) {
                if constexpr (std::is_same_v<Sample, float>) buffers[i].data32 = pointers[i].data();
                else buffers[i].data64 = pointers[i].data();
            }
        }
    }
    void bind(clap_process_t& proc) {
        proc.audio_inputs = inBuffers.empty() ? nullptr : inBuffers.data();
        proc.audio_inputs_count = static_cast<uint32_t>(inBuffers.size());
        proc.audio_outputs = outBuffers.empty() ? nullptr : outBuffers.data();
        proc.audio_outputs_count = static_cast<uint32_t>(outBuffers.size());
    }
    Sample* const* mainInput() { return mainIn >= 0 ? inPointers[static_cast<size_t>(mainIn)].data() : nullptr; }
    Sample* const* mainOutput() { return outPointers.empty() ? nullptr : outPointers[0].data(); }
    uint32_t width() const { return outPointers.empty() ? 0 : static_cast<uint32_t>(outPointers[0].size()); }
    // Every input but the main one: the sidechain.
    template <class Fn>
    void forEachSidechain(Fn&& fn) {
        for (size_t i = 0; i < inPointers.size(); ++i)
            if (static_cast<int>(i) != mainIn)
                for (auto* c : inPointers[i]) fn(c);
    }
    // Every output channel, main first.
    template <class Fn>
    void forEachOutput(Fn&& fn) {
        for (auto& port : outPointers)
            for (auto* c : port) fn(c);
    }
    void silence(uint32_t count) {
        for (auto& port : inPointers)
            for (auto* c : port) std::fill(c, c + count, Sample(0));
        forEachOutput([&](Sample* c) { std::fill(c, c + count, Sample(0)); });
    }
};

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
        // A level whose minimum reads "-inf" (off) takes "-inf" for exactly
        // that minimum; every other parameter refuses it.
        char minimum[256]{};
        const bool offAtMinimum = params->value_to_text(plug, info.id, info.min_value, minimum, sizeof(minimum)) &&
            !std::strncmp(minimum, "-inf", 4);
        if (offAtMinimum) {
            double decoded = 12345.0;
            if (!params->text_to_value(plug, info.id, "-inf", &decoded) || decoded != info.min_value) {
                std::printf("     \"-inf\" is not the minimum: id=%u %s decoded=%g\n", info.id, info.name, decoded);
                rejectsNonfinite = false;
            }
        }
        // Rejection, not merely a finite/clamped result, is the contract.
        for (const char* text : {"", " ", "garbage", "nan", "-nan", "NaN", "nan(1)", "nan dB", "inf",
                                 "-inf", "Inf", "infinity", "INFINITY", "1e999", "-1e999"}) {
            if (offAtMinimum && !std::strcmp(text, "-inf")) continue;
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
              uint32_t channels, double sampleRate, bool inPlace, bool notes) {
    Render result;
    RenderInstance instance(factory, id);
    const auto* plug = instance.plug;
    if (!plug || !selectLayout(plug, channels)) return result;
    const auto* params = static_cast<const clap_plugin_params_t*>(
        plug->get_extension(plug, CLAP_EXT_PARAMS));
    if (!params) return result;
    InputEvents events;
    bool configured = false;
    // The plain parameter moves off its default, so the renders run a setting
    // the product chose to exercise. Pin every instrument seed so two fresh
    // instances receive the same randomness.
    for (uint32_t i = 0; i < params->count(plug); ++i) {
        clap_param_info_t info{};
        if (!params->get_info(plug, i, &info)) return result;
        if (info.flags & CLAP_PARAM_IS_READONLY) continue;
        const bool plain = !std::strcmp(info.name, plainParam);
        const bool seed = notes && !std::strcmp(info.name, "Seed");
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
    // within that channel. Different signals per channel expose cross-talk.
    Buses<Sample> buses;
    buses.build(plug, 512, inPlace);
    const bool effect = buses.mainIn >= 0;
    std::vector<std::vector<Sample>> input(buses.width(), std::vector<Sample>(512));
    clap_process_t proc{};
    buses.bind(proc);
    proc.in_events = &events.input;
    proc.out_events = &events.output;
    constexpr uint32_t blocks[] = {1, 64, 257, 512};
    uint64_t frame = 0;
    result.ok = true;
    for (int b = 0; b < 32; ++b) {
        const uint32_t count = blocks[b % 4];
        proc.frames_count = count;
        proc.steady_time = static_cast<int64_t>(frame);
        // Quantize both paths' input to float, then widen for double.
        // Otherwise this test would include input quantization error.
        auto tone = [&](uint32_t c, uint32_t i, double gain, double hz) {
            const double t = static_cast<double>(frame + i) / sampleRate;
            return static_cast<Sample>(static_cast<float>(gain * std::sin(6.283185307179586 * hz * t + c)));
        };
        for (uint32_t c = 0; c < buses.width(); ++c)
            for (uint32_t i = 0; i < count; ++i)
                input[c][i] = tone(c, i, c == 0 ? 0.23 : 0.13, c == 0 ? 220.0 : 371.0 + 13.0 * c);
        if (Sample* const* main = buses.mainInput())
            for (uint32_t c = 0; c < buses.width(); ++c) std::copy(input[c].begin(), input[c].begin() + count, main[c]);
        uint32_t key = 0;
        buses.forEachSidechain([&](Sample* c) {
            for (uint32_t i = 0; i < count; ++i) c[i] = tone(key, i, (b / 8) % 2 ? 0.4 : 0.0, 97.0);
            ++key;
        });
        if (!inPlace)
            buses.forEachOutput([&](Sample* c) { std::fill(c, c + count, std::numeric_limits<Sample>::quiet_NaN()); });
        clap_event_note_t note{};
        events.events.clear();
        if (notes && (b == 1 || b == 5 || b == 13 || b == 17)) {
            note.header.size = sizeof(note);
            note.header.time = 7;
            note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            note.header.type = static_cast<uint16_t>((b == 1 || b == 13) ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
            note.note_id = b < 13 ? 1 : 2;
            note.port_index = 0;
            note.channel = 0;
            note.key = 36;
            note.velocity = 0.8;
            events.events = {&note.header};
        }
        if (plug->process(plug, &proc) == CLAP_PROCESS_ERROR) result.ok = false;
        uint32_t channel = 0;
        buses.forEachOutput([&](Sample* c) {
            for (uint32_t i = 0; i < count; ++i) {
                const double v = c[i];
                if (!std::isfinite(v)) result.ok = false;
                result.samples.push_back(v);
                result.peak = std::max(result.peak, std::fabs(v));
                if (effect && channel < buses.width())
                    result.inputDifference = std::max(result.inputDifference,
                        std::fabs(v - static_cast<double>(input[channel][i])));
            }
            ++channel;
        });
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

// A transparent effect (a meter, an analyser) must give back its input
// exactly, on both precision paths. Whether any other effect changes its
// input is reported, not checked: an effect may pass it through.
inline void processing(const clap_plugin_factory_t* factory, const char* id,
                       const char* plainParam, uint32_t channels, double sampleRate,
                       bool effect, bool notes, Check check, bool transparent = false) {
    // Each render starts with a fresh, identically configured instance. Comparing
    // successive calls on one live processor compares different DSP histories.
    const auto f = render<float>(factory, id, plainParam, channels, sampleRate, false, notes);
    const auto d = render<double>(factory, id, plainParam, channels, sampleRate, false, notes);
    if (channels == 0) {  // no audio: it ran, with notes in and nothing to hear
        check(f.ok && d.ok, "float and double processing runs without audio");
        return;
    }
    check(f.ok && f.peak > 1e-5, "float render writes finite, non-silent audio");
    check(d.ok && d.peak > 1e-5, "double render writes finite, non-silent audio");
    const double precisionError = difference(f, d);
    // Absolute sample tolerance, with exactly representable shared inputs.
    check(precisionError < 1e-5, "float/double full renders agree from identical initial state");
    std::printf("     float/double max error: %.9g; peaks: %.6g / %.6g\n",
                precisionError, f.peak, d.peak);
    if (!effect) return; // No input port means no in-place promise.
    std::printf("     largest difference from the input float/double: %.9g / %.9g\n",
                f.inputDifference, d.inputDifference);
    if (transparent)
        check(f.inputDifference == 0.0 && d.inputDifference == 0.0,
              "both effect precision paths pass the input through exactly");
    const auto fi = render<float>(factory, id, plainParam, channels, sampleRate, true, notes);
    const auto di = render<double>(factory, id, plainParam, channels, sampleRate, true, notes);
    const double floatError = difference(f, fi), doubleError = difference(d, di);
    check(floatError == 0.0, "float in-place matches separate buffers on every channel");
    check(doubleError == 0.0, "double in-place matches separate buffers on every channel");
    std::printf("     in-place errors float/double: %.9g / %.9g\n", floatError, doubleError);
}
} // namespace host_contract
