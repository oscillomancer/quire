// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// clap-bench <plugin.clap> <plugin-id> [--seconds S] [--runs N]
//            [--save DIR] [--compare DIR] [--only NAME]
//
// What a plugin costs, and whether a change kept its sound. Each setup
// renders S seconds (4 by default) of a stimulus at 48 kHz in stereo, in a
// fresh instance (an instrument, or anything with no audio input, plays
// overlapping notes as well):
//
//   defaults    the default patch, 512-frame blocks
//   small       the same in 32-frame blocks: the cost per block
//   automated   every writable, automatable parameter jumping to seeded
//               random values, as sample-accurate events, 512-frame blocks
//   <preset>    each factory patch, 512-frame blocks
//
// A cost is the fastest of N runs (5), printed as a share of one core in
// real time and as "refs": that time divided by a reference workload (eight
// biquads and a tanh per sample, in stereo) timed beside it, so a busy
// machine slows both alike. Compare refs between builds, not percentages.
//
// Every setup's output is hashed. --save DIR writes each as raw floats;
// --compare DIR reads them back and reports, per setup, whether the output
// is bit-identical and otherwise the largest difference and where it
// starts. Exit 0 = every setup rendered (and, with --compare, matched
// bit for bit); 1 = a difference or a failure; 2 = a usage error.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "supple/host/clap_host.h"
#include "supple/json/json.h"
#include "supple/json/write.h"

using namespace supple;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::uint32_t kMaxBlock = 4096;
constexpr double kPi = 3.14159265358979323846;

std::int64_t frames = 4 * 48000;
int runs = 5;
int failures = 0;

// Seeded noise under a 220 Hz tone, quiet for the last quarter; right is not
// left, so a stereo tool does not see mono.
struct Stimulus {
    std::vector<float> left, right;
};

Stimulus makeStimulus() {
    Stimulus s;
    s.left.resize(static_cast<std::size_t>(frames));
    s.right.resize(static_cast<std::size_t>(frames));
    std::mt19937 rng(0xbe9c4u);
    std::uniform_real_distribution<float> noise(-0.2f, 0.2f);
    for (std::int64_t i = 0; i < frames; ++i) {
        const double level = i < frames * 3 / 4 ? 1.0 : 0.01;
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

void sortByTime(std::vector<Timed>& events) {
    std::stable_sort(events.begin(), events.end(),
                     [](const Timed& x, const Timed& y) { return x.sample < y.sample; });
}

// A hundred jumps a second of the writable, automatable parameters, a
// stepped one on a step.
std::vector<Timed> makeAutomation(const std::vector<host::ParamInfo>& params) {
    std::vector<Timed> events;
    if (params.empty()) return events;
    std::mt19937 rng(0xa070u);
    std::uniform_int_distribution<std::int64_t> frame(0, frames - 1);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const auto count = frames * 100 / static_cast<std::int64_t>(kSampleRate);
    for (std::int64_t k = 0; k < count; ++k) {
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

// Overlapping notes, six a second, for an instrument.
std::vector<Timed> makeNotes() {
    std::vector<Timed> events;
    std::mt19937 rng(0x4073u);
    const auto count = static_cast<int>(frames * 6 / static_cast<std::int64_t>(kSampleRate));
    for (int k = 0; k < count; ++k) {
        const auto start = static_cast<std::int64_t>(
            rng() % static_cast<std::uint32_t>(std::max<std::int64_t>(frames - 8000, 1)));
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

struct Setup {
    std::string name;
    std::string patch;  // a factory patch, compact JSON; empty for the defaults
    std::uint32_t block = 512;
    bool automated = false;
};

struct Render {
    std::vector<float> left, right;
    double seconds = 0.0;  // the time spent in process()
    bool ok = false;
};

// One fresh instance renders the whole stimulus in `block`-frame blocks,
// each event at its exact frame; only process() is timed.
Render render(const host::PluginInfo& info, const Setup& setup, const Stimulus& stimulus,
              const std::vector<Timed>& notes) {
    Render out;
    auto created = host::Instance::create(info, {});
    if (!created) return out;
    host::Instance& in = **created;
    if (!setup.patch.empty() && !in.setPatch(setup.patch, CLAP_SUPPLE_SET_REPLACE).ok()) return out;
    if (!in.activate(kSampleRate, 1, kMaxBlock).ok()) return out;
    std::vector<Timed> events = notes;
    if (setup.automated) {
        std::vector<host::ParamInfo> params;
        for (const auto& p : in.params())
            if ((p.flags & CLAP_PARAM_IS_AUTOMATABLE) && !(p.flags & CLAP_PARAM_IS_READONLY))
                params.push_back(p);
        const auto automation = makeAutomation(params);
        events.insert(events.end(), automation.begin(), automation.end());
        sortByTime(events);
    }

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
    out.left.assign(static_cast<std::size_t>(frames), 0.0f);
    out.right.assign(static_cast<std::size_t>(frames), 0.0f);

    if (!in.startProcessing()) return out;
    std::size_t next = 0;
    bool processed = true;
    std::chrono::steady_clock::duration spent{};
    for (std::int64_t pos = 0; pos < frames;) {
        const auto n =
            static_cast<std::uint32_t>(std::min<std::int64_t>(setup.block, frames - pos));
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
        const auto start = std::chrono::steady_clock::now();
        if (!in.process(b, &outEvents)) processed = false;
        spent += std::chrono::steady_clock::now() - start;
        std::copy_n(outL.begin(), n, out.left.begin() + pos);
        std::copy_n(outR.begin(), n, out.right.begin() + pos);
        pos += n;
    }
    in.stopProcessing();
    out.seconds = std::chrono::duration<double>(spent).count();
    out.ok = processed;
    return out;
}

// Eight biquads and a tanh per sample, in stereo, over the stimulus.
double reference(const Stimulus& stimulus) {
    double best = std::numeric_limits<double>::infinity();
    for (int k = 0; k < runs; ++k) {
        double z[2][8][2] = {};
        double sum = 0.0;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < stimulus.left.size(); ++i) {
            for (int c = 0; c < 2; ++c) {
                double x = static_cast<double>(c == 0 ? stimulus.left[i] : stimulus.right[i]);
                for (auto& s : z[c]) {
                    const double y = 0.2 * x + s[0];
                    s[0] = 0.4 * x - 0.3 * y + s[1];
                    s[1] = 0.2 * x - 0.1 * y;
                    x = y;
                }
                sum += std::tanh(x);
            }
        }
        best = std::min(
            best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        if (sum == 12345.678) std::printf(" ");  // keeps the loop
    }
    return best;
}

std::uint64_t hashOf(const Render& r) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::vector<float>& v) {
        for (float f : v) {
            std::uint32_t bits;
            std::memcpy(&bits, &f, sizeof(bits));
            for (int b = 0; b < 4; ++b) {
                h ^= (bits >> (8 * b)) & 0xffu;
                h *= 1099511628211ull;
            }
        }
    };
    mix(r.left);
    mix(r.right);
    return h;
}

std::string fileName(const std::string& name) {
    std::string out;
    for (char c : name)
        out +=
            std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(c)) : '-';
    return out + ".f32";
}

void save(const std::filesystem::path& dir, const std::string& name, const Render& r) {
    std::filesystem::create_directories(dir);
    std::ofstream f(dir / fileName(name), std::ios::binary);
    f.write(reinterpret_cast<const char*>(r.left.data()),
            static_cast<std::streamsize>(r.left.size() * sizeof(float)));
    f.write(reinterpret_cast<const char*>(r.right.data()),
            static_cast<std::streamsize>(r.right.size() * sizeof(float)));
}

// "identical", or how the output differs from the one saved in dir.
std::string compare(const std::filesystem::path& dir, const std::string& name, const Render& r,
                    bool& same) {
    same = false;
    std::ifstream f(dir / fileName(name), std::ios::binary);
    if (!f) return "nothing saved to compare with";
    std::vector<float> saved(r.left.size() + r.right.size());
    f.read(reinterpret_cast<char*>(saved.data()),
           static_cast<std::streamsize>(saved.size() * sizeof(float)));
    if (static_cast<std::size_t>(f.gcount()) != saved.size() * sizeof(float))
        return "the saved output has another length";
    double worst = 0.0;
    std::int64_t first = -1;
    for (std::size_t i = 0; i < saved.size(); ++i) {
        const float now = i < r.left.size() ? r.left[i] : r.right[i - r.left.size()];
        if (std::memcmp(&now, &saved[i], sizeof(float)) == 0) continue;
        if (first < 0) first = static_cast<std::int64_t>(i % r.left.size());
        worst = std::max(worst, std::abs(static_cast<double>(now) - static_cast<double>(saved[i])));
    }
    if (first < 0) {
        same = true;
        return "identical";
    }
    char text[160];
    std::snprintf(text, sizeof(text), "differs from frame %lld, at most %.1f dBFS (%.3g)",
                  static_cast<long long>(first),
                  worst > 0.0 ? 20.0 * std::log10(worst) : -std::numeric_limits<double>::infinity(),
                  worst);
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path savePath, comparePath;
    std::string only;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--seconds" && hasValue)
            frames = static_cast<std::int64_t>(std::atof(argv[++i]) * kSampleRate);
        else if (a == "--runs" && hasValue)
            runs = std::max(1, std::atoi(argv[++i]));
        else if (a == "--save" && hasValue)
            savePath = argv[++i];
        else if (a == "--compare" && hasValue)
            comparePath = argv[++i];
        else if (a == "--only" && hasValue)
            only = argv[++i];
        else
            positional.push_back(a);
    }
    if (positional.size() != 2 || frames < 4096) {
        std::fprintf(stderr,
                     "usage: %s <plugin.clap> <plugin-id> [--seconds S] [--runs N] [--save DIR] "
                     "[--compare DIR] [--only NAME]\n",
                     argv[0]);
        return 2;
    }
    const std::filesystem::path path = positional[0];
    auto list = host::scan(std::span<const std::filesystem::path>(&path, 1));
    if (!list) {
        std::fprintf(stderr, "scan failed: %s\n", list.error().toString().c_str());
        return 2;
    }
    const auto found = std::find_if(list->begin(), list->end(), [&](const host::PluginInfo& p) {
        return p.id == positional[1];
    });
    if (found == list->end()) {
        std::fprintf(stderr, "no plugin %s in %s\n", positional[1].c_str(), path.string().c_str());
        return 2;
    }
    const host::PluginInfo& info = *found;

    std::vector<Setup> setups = {
        {"defaults", {}, 512, false}, {"small", {}, 32, false}, {"automated", {}, 512, true}};
    bool needsNotes = info.instrument;
    {
        auto probe = host::Instance::create(info, {});
        if (!probe) {
            std::fprintf(stderr, "create failed: %s\n", probe.error().toString().c_str());
            return 1;
        }
        needsNotes = needsNotes || (*probe)->audioInputChannels() == 0;
        if (auto patches = (*probe)->factoryPatches(); patches && patches->isArray())
            for (const auto& p : patches->asArray())
                if (const auto* name = p.find("name"); name && name->isString())
                    setups.push_back({name->asString(), json::writeCompact(p), 512, false});
    }
    const Stimulus stimulus = makeStimulus();
    const std::vector<Timed> notes = needsNotes ? makeNotes() : std::vector<Timed>{};
    const double realtime = static_cast<double>(frames) / kSampleRate;

    std::printf("%s, %.1f s at 48 kHz, fastest of %d\n", info.id.c_str(), realtime, runs);
    std::printf("%-28s %9s %8s  %-16s %s\n", "setup", "core %", "refs", "hash",
                comparePath.empty() ? "" : "output");
    double patchRefs = 0.0;
    int patchCount = 0;
    bool allSame = true;
    for (std::size_t k = 0; k < setups.size(); ++k) {
        const Setup& setup = setups[k];
        if (!only.empty() && setup.name != only) continue;
        Render best;
        best.seconds = std::numeric_limits<double>::infinity();
        bool ok = true;
        for (int r = 0; r < runs && ok; ++r) {
            Render now = render(info, setup, stimulus, notes);
            ok = now.ok;
            if (ok && now.seconds < best.seconds) best = std::move(now);
        }
        if (!ok) {
            std::printf("%-28s FAIL: could not render\n", setup.name.c_str());
            ++failures;
            continue;
        }
        bool finite = true;
        for (std::size_t i = 0; i < best.left.size() && finite; ++i)
            finite = std::isfinite(best.left[i]) && std::isfinite(best.right[i]);
        const double refs = best.seconds / reference(stimulus);
        if (k >= 3) {
            patchRefs += refs;
            ++patchCount;
        }
        std::string verdict;
        if (!comparePath.empty()) {
            bool same = false;
            verdict = compare(comparePath, setup.name, best, same);
            allSame = allSame && same;
        }
        if (!finite) {
            verdict += " FAIL: not finite";
            ++failures;
        }
        std::printf("%-28s %8.3f%% %8.3f  %016llx %s\n", setup.name.c_str(),
                    100.0 * best.seconds / realtime, refs,
                    static_cast<unsigned long long>(hashOf(best)), verdict.c_str());
        if (!savePath.empty()) save(savePath, setup.name, best);
    }
    if (patchCount > 0)
        std::printf("%-28s %9s %8.3f  (mean of %d factory patches)\n", "factory patches", "",
                    patchRefs / patchCount, patchCount);
    if (!comparePath.empty())
        std::printf("%s\n", allSame ? "every output is bit-identical to the saved one"
                                    : "some output differs from the saved one");
    return failures || !allSame ? 1 : 0;
}
