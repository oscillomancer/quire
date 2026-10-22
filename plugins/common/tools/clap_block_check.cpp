// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// clap-block-check <plugin.clap> [plugin-id]
//
// The engine's real-time output is bit-identical to its offline render only
// for plugins whose output does not depend on the host's block size
// (docs/specs/engine.md, docs/specs/effects.md §1.7): the device decides the
// block, and the engine splits blocks at its own boundaries too. A plugin
// that works out something once per process() call (an LFO stepped per
// block, a smoother advanced per call, coefficients from the block's length)
// breaks that.
//
// This renders two seconds of a stimulus with every writable, automatable
// parameter jumping to seeded random values at seeded frames, delivered as
// sample-accurate events, once in blocks of 4096 and once in blocks cycling
// through 1, 7, 64, 513, 2 and 1000 frames, and requires the two outputs to be
// bit-identical. An instrument (or anything with no audio input) is given
// notes as well. Every stock plugin registers this as plugin.<id>.blocks.
// Exit 0 = every plugin in the file passed.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "supple/host/clap_host.h"

using namespace supple;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::int64_t kFrames = 96000;  // two seconds
constexpr std::uint32_t kMaxBlock = 4096;
constexpr int kAutomationEvents = 400;
constexpr double kPi = 3.14159265358979323846;

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// Seeded noise under a 220 Hz tone, quiet for the last quarter so a gate or
// an expander has something to close on; right is not left, so a stereo tool
// does not see mono.
struct Stimulus {
    std::vector<float> left, right;
};

Stimulus makeStimulus() {
    Stimulus s;
    s.left.resize(kFrames);
    s.right.resize(kFrames);
    std::mt19937 rng(0x0b10c5u);
    std::uniform_real_distribution<float> noise(-0.2f, 0.2f);
    for (std::int64_t i = 0; i < kFrames; ++i) {
        const double level = i < kFrames * 3 / 4 ? 1.0 : 0.01;
        const auto tone = static_cast<float>(
            0.3 * std::sin(2.0 * kPi * 220.0 * static_cast<double>(i) / kSampleRate));
        s.left[static_cast<std::size_t>(i)] = static_cast<float>(level) * (tone + noise(rng));
        s.right[static_cast<std::size_t>(i)] = static_cast<float>(level) * (-tone + noise(rng));
    }
    return s;
}

struct Timed {
    std::int64_t sample = 0;
    host::Event event;
};

// Every writable, automatable parameter: a seeded random value at a seeded
// frame, a stepped one on a step.
std::vector<Timed> makeAutomation(const std::vector<host::ParamInfo>& params) {
    std::vector<Timed> events;
    if (params.empty()) return events;
    std::mt19937 rng(0xa070u);
    std::uniform_int_distribution<std::int64_t> frame(0, kFrames - 1);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int k = 0; k < kAutomationEvents; ++k) {
        const host::ParamInfo& p = params[rng() % params.size()];
        double v = p.min + (p.max - p.min) * unit(rng);
        if (p.flags & CLAP_PARAM_IS_STEPPED) v = std::clamp(std::round(v), p.min, p.max);
        host::Event e;
        e.kind = host::Event::Kind::Param;
        e.paramId = p.id;
        e.value = v;
        events.push_back({frame(rng), e});
    }
    return events;
}

// Overlapping notes across the render, for an instrument.
std::vector<Timed> makeNotes() {
    std::vector<Timed> events;
    std::mt19937 rng(0x4073u);
    for (int k = 0; k < 24; ++k) {
        const auto start =
            static_cast<std::int64_t>(rng() % static_cast<std::uint32_t>(kFrames - 8000));
        host::Event on;
        on.kind = host::Event::Kind::NoteOn;
        on.key = static_cast<std::int16_t>(40 + rng() % 40);
        on.noteId = k;
        on.velocity = 0.4 + static_cast<double>(rng() % 60) / 100.0;
        host::Event off = on;
        off.kind = host::Event::Kind::NoteOff;
        off.velocity = 0.0;
        events.push_back({start, on});
        events.push_back({start + 1 + static_cast<std::int64_t>(rng() % 7000), off});
    }
    return events;
}

struct Capture {
    std::vector<float> left, right;
    bool ok = false;
};

// Renders the whole stimulus in blocks cycling through `blocks`, each event
// delivered at its exact frame.
Capture render(host::Instance& in, const Stimulus& stimulus,
               const std::vector<std::uint32_t>& blocks, const std::vector<Timed>& events) {
    Capture out;
    const auto outChannels = in.audioOutputChannels();
    const auto inChannels = in.audioInputChannels();
    if (outChannels < 1) return out;

    std::vector<float> outL(kMaxBlock), outR(kMaxBlock), inL(kMaxBlock), inR(kMaxBlock);
    float* outPtr[2] = {outL.data(), outR.data()};
    float* inPtr[2] = {inL.data(), inR.data()};
    std::vector<host::OutputEvent> outStorage(1024);
    host::OutputEvents outEvents;
    outEvents.storage = outStorage;
    std::vector<host::Event> blockEvents;
    blockEvents.reserve(events.size());
    out.left.assign(static_cast<std::size_t>(kFrames), 0.0f);
    out.right.assign(static_cast<std::size_t>(kFrames), 0.0f);

    if (!in.startProcessing()) return out;
    std::size_t next = 0, cycle = 0;
    bool processed = true;
    for (std::int64_t pos = 0; pos < kFrames;) {
        const auto n = static_cast<std::uint32_t>(
            std::min<std::int64_t>(blocks[cycle++ % blocks.size()], kFrames - pos));
        for (std::uint32_t i = 0; i < n; ++i) {
            inL[i] = stimulus.left[static_cast<std::size_t>(pos) + i];
            inR[i] = stimulus.right[static_cast<std::size_t>(pos) + i];
        }
        blockEvents.clear();
        while (next < events.size() && events[next].sample < pos + n) {
            host::Event e = events[next].event;
            e.time = static_cast<std::uint32_t>(events[next].sample - pos);
            blockEvents.push_back(e);
            ++next;
        }
        host::ProcessBlock b;
        b.inputs = inChannels > 0 ? inPtr : nullptr;
        b.inputChannels = std::min<std::uint32_t>(inChannels, 2u);
        b.outputs = outPtr;
        b.outputChannels = std::min<std::uint32_t>(outChannels, 2u);
        b.frames = n;
        b.events = blockEvents;
        b.steadyTime = pos;
        outEvents.clear();
        if (!in.process(b, &outEvents)) processed = false;
        std::copy_n(outL.begin(), n, out.left.begin() + pos);
        std::copy_n(outR.begin(), n, out.right.begin() + pos);
        pos += n;
    }
    in.stopProcessing();
    out.ok = processed;
    return out;
}

bool allFinite(const Capture& c) {
    for (std::size_t i = 0; i < c.left.size(); ++i)
        if (!std::isfinite(c.left[i]) || !std::isfinite(c.right[i])) return false;
    return true;
}

double peakOf(const Capture& c) {
    double peak = 0.0;
    for (std::size_t i = 0; i < c.left.size(); ++i)
        peak = std::max({peak, std::abs(static_cast<double>(c.left[i])),
                         std::abs(static_cast<double>(c.right[i]))});
    return peak;
}

std::int64_t firstDifference(const Capture& a, const Capture& b) {
    for (std::size_t i = 0; i < a.left.size(); ++i)
        if (a.left[i] != b.left[i] || a.right[i] != b.right[i]) return static_cast<std::int64_t>(i);
    return -1;
}

void testPlugin(const host::PluginInfo& info) {
    std::printf("%s (%s)\n", info.id.c_str(), info.instrument ? "instrument" : "effect");
    const Stimulus stimulus = makeStimulus();
    auto a = host::Instance::create(info, {});
    auto b = host::Instance::create(info, {});
    if (!a || !b || !(*a)->activate(kSampleRate, 1, kMaxBlock).ok() ||
        !(*b)->activate(kSampleRate, 1, kMaxBlock).ok()) {
        check(false, "create and activate");
        return;
    }
    std::vector<host::ParamInfo> params;
    for (const auto& p : (*a)->params())
        if ((p.flags & CLAP_PARAM_IS_AUTOMATABLE) && !(p.flags & CLAP_PARAM_IS_READONLY))
            params.push_back(p);
    std::vector<Timed> events = makeAutomation(params);
    if (info.instrument || (*a)->audioInputChannels() == 0) {
        const auto notes = makeNotes();
        events.insert(events.end(), notes.begin(), notes.end());
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const Timed& x, const Timed& y) { return x.sample < y.sample; });

    const Capture whole = render(**a, stimulus, {kMaxBlock}, events);
    const Capture split = render(**b, stimulus, {1, 7, 64, 513, 2, 1000}, events);
    char detail[256];
    std::snprintf(detail, sizeof(detail), "%zu parameters, %zu events, peak %.9g", params.size(),
                  events.size(), peakOf(whole));
    std::string what =
        std::string("output is independent of the host's block size under automation: ") + detail;
    if (!whole.ok || !split.ok) {
        check(false, what + "; a render reported a processing error");
        return;
    }
    if (!allFinite(whole) || !allFinite(split)) {
        check(false, what + "; the output is not finite");
        return;
    }
    if (peakOf(whole) <= 0.0) {
        check(false, what + "; the render is silent, so the comparison proves nothing");
        return;
    }
    const auto differing = firstDifference(whole, split);
    if (differing >= 0) what += "; the renders differ from sample " + std::to_string(differing);
    check(differing < 0, what);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <plugin.clap> [plugin-id]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path path = argv[1];
    const std::string only = argc == 3 ? argv[2] : "";
    auto list = host::scan(std::span<const std::filesystem::path>(&path, 1));
    if (!list) {
        std::fprintf(stderr, "scan failed: %s\n", list.error().toString().c_str());
        return 2;
    }
    int tested = 0;
    for (const auto& info : *list) {
        if (!only.empty() && info.id != only) continue;
        ++tested;
        testPlugin(info);
    }
    if (tested == 0) {
        std::fprintf(stderr, "no plugin found in %s\n", path.string().c_str());
        return 2;
    }
    std::printf("%s: %d failure(s)\n", path.filename().string().c_str(), failures);
    return failures ? 1 : 0;
}
