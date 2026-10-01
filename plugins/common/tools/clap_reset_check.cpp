// clap-reset-check <plugin.clap> [plugin-id]
//
// CLAP's contract: reset() clears all processing state (filters, oscillators,
// envelopes, LFOs, smoothers, delay lines) and keeps the parameter values. The
// sandbox and the engine reuse one instance across renders, so a render must
// depend only on the state set for it, never on the render before it.
//
// This drives the real host library the way host/sandbox/src/render.cpp does
// (reset(), flush the queued parameter changes, start, process a whole second,
// stop) and requires two renders that end in the same state to be
// bit-identical:
//
//   A: activate, state S1, render 1 s; state S2, reset(), render 1 s
//   B: fresh instance, activate, state S2, render 1 s
//
// A's second render and B's render must match exactly, at block 256 and again
// at block 64. Every stock plugin registers this as plugin.<id>.reset.
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
constexpr int kRenderSeconds = 1;
constexpr double kPi = 3.14159265358979323846;

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// A deterministic stimulus: a 220 Hz tone with a short seeded noise burst, so
// an effect sees both steady-state and transient material. Right is the
// inverse of left, which keeps a stereo tool from seeing a mono signal.
struct Stimulus {
    std::vector<float> left, right;
};

Stimulus makeStimulus() {
    const auto frames = static_cast<std::size_t>(kSampleRate) * kRenderSeconds;
    const auto burst = static_cast<std::size_t>(kSampleRate) / 20;  // 50 ms
    Stimulus s;
    s.left.resize(frames);
    s.right.resize(frames);
    std::mt19937 rng(0x5eed1234u);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        float v = static_cast<float>(0.25 * std::sin(2.0 * kPi * 220.0 * t));
        if (i < burst) v += noise(rng);
        s.left[i] = v;
        s.right[i] = -v;
    }
    return s;
}

struct Timed {
    std::int64_t sample = 0;
    host::Event event;
};

// An instrument needs notes; the exact figure does not matter as long as both
// renders are given the same one.
std::vector<Timed> makeNotes() {
    std::vector<Timed> events;
    auto note = [&](std::int64_t at, bool on, std::int16_t key) {
        host::Event e;
        e.kind = on ? host::Event::Kind::NoteOn : host::Event::Kind::NoteOff;
        e.port = 0;
        e.channel = 0;
        e.key = key;
        e.noteId = -1;
        e.velocity = on ? 0.8 : 0.0;
        events.push_back({at, e});
    };
    note(0, true, 60);
    note(24000, false, 60);
    note(24000, true, 67);
    note(47998, false, 67);
    return events;
}

struct Capture {
    std::vector<float> left, right;
    bool ok = false;
};

// One "render" in the sandbox's sense: reset, deliver the queued parameter
// changes, start, process a second, stop. The output is the block stream the
// plugin produced, latency included (identical for both instances).
Capture renderSecond(host::Instance& in, const Stimulus& stimulus, std::uint32_t block,
                     const std::vector<Timed>& events) {
    Capture out;
    const auto frames = static_cast<std::int64_t>(kSampleRate) * kRenderSeconds;
    const auto outChannels = in.audioOutputChannels();
    const auto inChannels = in.audioInputChannels();
    if (outChannels < 1) return out;

    std::vector<float> outL(block, 0.0f), outR(block, 0.0f), inL(block, 0.0f), inR(block, 0.0f);
    float* outPtr[2] = {outL.data(), outR.data()};
    float* inPtr[2] = {inL.data(), inR.data()};
    std::vector<host::OutputEvent> outStorage(1024);
    host::OutputEvents outEvents;
    outEvents.storage = outStorage;
    std::vector<host::Event> blockEvents;
    blockEvents.reserve(events.size() + 1);

    out.left.assign(static_cast<std::size_t>(frames), 0.0f);
    out.right.assign(static_cast<std::size_t>(frames), 0.0f);

    in.reset();
    in.flushParams();
    if (!in.startProcessing()) return out;

    std::size_t next = 0;
    bool processed = true;
    for (std::int64_t pos = 0; pos < frames; pos += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::int64_t>(block, frames - pos));
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto at = static_cast<std::size_t>(pos) + i;
            inL[i] = at < stimulus.left.size() ? stimulus.left[at] : 0.0f;
            inR[i] = at < stimulus.right.size() ? stimulus.right[at] : 0.0f;
        }
        blockEvents.clear();
        while (next < events.size() && events[next].sample < pos + n) {
            host::Event e = events[next].event;
            e.time =
                static_cast<std::uint32_t>(std::max<std::int64_t>(0, events[next].sample - pos));
            blockEvents.push_back(e);
            ++next;
        }
        host::ProcessBlock b;
        b.inputs = inChannels > 0 ? inPtr : nullptr;
        b.inputChannels = inChannels > 0 ? std::min<std::uint32_t>(inChannels, 2u) : 0u;
        b.outputs = outPtr;
        b.outputChannels = std::min<std::uint32_t>(outChannels, 2u);
        b.frames = n;
        b.events = blockEvents;
        b.steadyTime = pos;
        outEvents.clear();
        if (!in.process(b, &outEvents)) processed = false;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto at = static_cast<std::size_t>(pos) + i;
            out.left[at] = outL[i];
            out.right[at] = outR[i];
        }
    }
    in.stopProcessing();
    out.ok = processed;
    return out;
}

double peakOf(const Capture& c) {
    double peak = 0.0;
    for (std::size_t i = 0; i < c.left.size(); ++i)
        peak = std::max({peak, std::abs(static_cast<double>(c.left[i])),
                         std::abs(static_cast<double>(c.right[i]))});
    return peak;
}

// The first sample where the two captures differ, or -1 when identical.
std::int64_t firstDifference(const Capture& a, const Capture& b) {
    for (std::size_t i = 0; i < a.left.size(); ++i) {
        if (a.left[i] != b.left[i] || a.right[i] != b.right[i]) return static_cast<std::int64_t>(i);
    }
    return -1;
}

std::vector<host::ParamInfo> continuousParams(host::Instance& in) {
    std::vector<host::ParamInfo> out;
    for (const auto& p : in.params())
        if (!(p.flags & CLAP_PARAM_IS_STEPPED) && !(p.flags & CLAP_PARAM_IS_READONLY))
            out.push_back(p);
    return out;
}

// A value this parameter's default is not, so that the "before" state is
// genuinely different from the "after" state.
double nonDefaultValue(const host::ParamInfo& p) {
    for (double fraction : {0.25, 0.75, 0.5}) {
        const double v = p.min + (p.max - p.min) * fraction;
        if (v != p.def) return v;
    }
    return p.def;  // min == max: nothing to vary
}

bool applyAll(host::Instance& in, const std::vector<host::ParamInfo>& params,
              const std::vector<double>& values, std::string& error) {
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto status = in.setParam(params[i].id, values[i]);
        if (!status.ok()) {
            error = params[i].name + ": " + status.error().toString();
            return false;
        }
    }
    return true;
}

// Compares the two instances at one block size. `detail` carries what the
// comparison actually covered (parameter count, peaks, the first differing
// sample) so a failure can be read without reproducing it.
bool compareAtBlock(const host::PluginInfo& info, std::uint32_t block, int pass,
                    std::string& detail) {
    const Stimulus stimulus = makeStimulus();

    auto a = host::Instance::create(info, {});
    auto b = host::Instance::create(info, {});
    if (!a || !b) {
        detail = "create failed";
        return false;
    }
    if (!(*a)->activate(kSampleRate, 1, block).ok() ||
        !(*b)->activate(kSampleRate, 1, block).ok()) {
        detail = "activate failed";
        return false;
    }
    if ((*a)->audioOutputChannels() < 1 || (*b)->audioOutputChannels() < 1) {
        detail = "the plugin has no audio output";
        return false;
    }
    const auto params = continuousParams(**a);
    // An instrument (or anything with no audio input) is only driven by notes.
    const auto notes =
        (info.instrument || (*a)->audioInputChannels() == 0) ? makeNotes() : std::vector<Timed>{};

    // S1 moves every continuous parameter off its default; S2 is the default
    // state a fresh instance already holds, so the reference render is the
    // plugin's own normal state (an audible one, whatever the plugin is).
    std::vector<double> s1, s2;
    s1.reserve(params.size());
    s2.reserve(params.size());
    bool differ = false;
    for (const auto& p : params) {
        s1.push_back(nonDefaultValue(p));
        s2.push_back(std::clamp(p.def, p.min, p.max));
        differ = differ || s1.back() != s2.back();
    }

    std::string error;
    if (!applyAll(**a, params, s1, error) || !applyAll(**b, params, s2, error)) {
        detail = "setParam failed: " + error;
        return false;
    }
    const Capture before = renderSecond(**a, stimulus, block, notes);
    if (!before.ok) {
        detail = "the first render reported a processing error";
        return false;
    }
    if (!applyAll(**a, params, s2, error)) {
        detail = "setParam failed: " + error;
        return false;
    }
    const Capture afterReset = renderSecond(**a, stimulus, block, notes);
    const Capture fresh = renderSecond(**b, stimulus, block, notes);
    if (!afterReset.ok || !fresh.ok) {
        detail = "a render reported a processing error";
        return false;
    }

    char summary[256];
    std::snprintf(summary, sizeof(summary),
                  "[pass %d block %u] %zu continuous params, peaks %.9g/%.9g", pass, block,
                  params.size(), peakOf(afterReset), peakOf(fresh));
    detail = summary;

    // Guard against a comparison that proves nothing: the reference has to
    // carry signal, and moving every parameter off its default has to be
    // audible somewhere in the render.
    if (peakOf(fresh) <= 0.0) {
        detail += "; the reference render is silent";
        return false;
    }
    if (differ && firstDifference(before, fresh) < 0) {
        detail += "; the S1 and S2 renders are identical, so the comparison proves nothing";
        return false;
    }

    const auto differing = firstDifference(afterReset, fresh);
    if (differing >= 0) {
        detail += "; the S2 renders differ from sample " + std::to_string(differing);
        return false;
    }
    return true;
}

void testPlugin(const host::PluginInfo& info) {
    std::printf("%s (%s)\n", info.id.c_str(), info.instrument ? "instrument" : "effect");
    for (int pass = 0; pass < 2; ++pass) {
        const std::uint32_t block = pass == 0 ? 256u : 64u;
        std::string detail;
        const bool ok = compareAtBlock(info, block, pass, detail);
        std::string what =
            "a render after reset depends only on the state (block " + std::to_string(block) + ")";
        if (!detail.empty()) what += ": " + detail;
        check(ok, what);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <plugin.clap> [plugin-id]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path path = argv[1];
    const std::string only = argc == 3 ? argv[2] : std::string();
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
        std::fprintf(stderr, "no plugin found in %s\n", path.c_str());
        return 2;
    }
    std::printf("%s: %d failure(s)\n", path.filename().c_str(), failures);
    return failures ? 1 : 0;
}
