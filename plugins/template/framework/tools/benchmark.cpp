// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What the product costs, measured, against what it cost before:
//
//   benchmark             measure, and compare with product/benchmark.txt
//   benchmark --update    record this machine's measurements there
//
// Each scenario renders four seconds through the Engine the plugin runs, at
// 48 kHz in stereo (an instrument plays an eight-note chord, a note effect
// takes a note every 100 ms, a sidechain is fed):
//
//   steady     the defaults, 512-frame blocks
//   small      the same in 32-frame blocks: the cost per block
//   automated  every parameter moved every 64 frames, as automation does
//
// A cost is the fastest of five runs, divided by a reference workload timed
// alongside it (eight biquads and a tanh per sample, in stereo), so a busy
// or throttled machine slows both alike and the number is "this many
// references". It is printed as a share of one core in real time too.
//
// The baseline keeps one set of numbers per machine (processor and
// compiler). On a machine it has none for, a run reports and skips (exit
// 77) rather than compare with somebody else's computer. A scenario costing
// more than 1.5 times its baseline (and more than a twentieth of the
// reference over it, so a near-free product is not failed by timer noise)
// fails: a real regression, not noise -- a filter rebuilt per sample,
// denormals, an allocation, an O(n^2) loop.
// After a change that costs more on purpose, --update and commit the file.
#include "tools/Timing.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "Testing.h"
#include "dsp/Filters.h"

using namespace supple;

namespace {

constexpr double kRate = 48000.0;
constexpr int kFrames = static_cast<int>(4 * kRate);
constexpr int kRuns = 5;
constexpr double kTolerance = 1.5;
constexpr double kFloor = 0.05;  // references: below this, timing noise is the measurement

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

// This processor and this compiler: what the numbers belong to.
std::string machine() {
    std::string cpu;
#if defined(__APPLE__)
    char name[256] = {};
    size_t size = sizeof(name);
    if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0) cpu = name;
#elif defined(_WIN32)
    if (const char* id = std::getenv("PROCESSOR_IDENTIFIER")) cpu = id;
#else
    std::ifstream info("/proc/cpuinfo");
    for (std::string line; std::getline(info, line);)
        if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) {
            cpu = line.substr(line.find(':') + 1);
            break;
        }
#endif
    std::string compiler;
#if defined(__clang__)
    compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
    compiler = "GCC " __VERSION__;
#elif defined(_MSC_VER)
    compiler = "MSVC " + std::to_string(_MSC_VER);
#endif
    if (compiler.find(' ', compiler.find(' ') + 1) != std::string::npos)  // "clang 20.1.8 (https://...)"
        compiler = compiler.substr(0, compiler.find(' ', compiler.find(' ') + 1));
    return trim(cpu.empty() ? "unknown processor" : cpu) + " | " + compiler;
}

template <typename F>
double fastest(F&& run) {
    double best = INFINITY;
    for (int k = 0; k < kRuns; ++k) {
        const auto start = std::chrono::steady_clock::now();
        run();
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    return best;
}

// Eight biquads and a tanh per sample, stereo, for as long as a scenario.
double reference() {
    const dsp::Biquad b = dsp::Biquad::lowpass(5000.0, 0.7071, kRate);
    std::vector<double> x(static_cast<size_t>(kFrames));
    std::minstd_rand rng(1);
    volatile double sink = 0.0;
    return fastest([&] {
        for (int channel = 0; channel < 2; ++channel) {
            for (auto& v : x) v = static_cast<double>(rng() % 2001) / 1000.0 - 1.0;
            dsp::Biquad::State s[8];
            for (double& v : x) {
                double y = v;
                for (auto& state : s) y = b.tick(state, y);
                v = std::tanh(2.0 * y);
            }
            sink = sink + x[static_cast<size_t>(kFrames / 2)];
        }
    });
}

struct Scenario {
    const char* name;
    int block;
    bool automated;
};

double measure(const Scenario& scenario) {
    test::RenderOptions o;
    o.sampleRate = kRate;
    o.blocks = {scenario.block};
    o.frames = kFrames;
    const int channels = product::info::kAudioOutput ? product::info::kChannelLayouts[0] : 0;
    const test::Signal input = channels ? test::noise(channels, kFrames, 3, 0.25) : test::Signal{};
    if (product::info::kSidechain) o.sidechain = test::noise(channels, kFrames, 5, 0.25);
    if (product::info::kNoteInput) {
        const int chord[] = {48, 55, 60, 64, 67, 72, 76, 79};
        const int every = product::info::kInstrument ? static_cast<int>(kRate) : static_cast<int>(kRate / 10);
        for (int frame = 0; frame < kFrames; frame += every)
            for (int key : chord) {
                if (frame > 0) o.events.push_back(test::noteOff(frame, key));
                o.events.push_back(test::noteOn(frame, key, 0.8));
                if (!product::info::kInstrument) break;
            }
    }
    if (scenario.automated) {
        std::mt19937 rng(9);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (int frame = 0; frame < kFrames; frame += 64)
            for (size_t i = 0; i < params::kCount; ++i) {
                const ParamDef& p = params::at(i);
                if (!p.is(pflag::kAutomatable) || p.is(pflag::kBypass) || p.is(pflag::kRestart) || p.isOutput()) continue;
                o.automation.push_back({frame, p.id, toPlain(p, fromNormalized(p, unit(rng)))});
            }
    }
    std::unique_ptr<Engine> engine;
    test::RenderOptions warm = o;
    warm.frames = 512;
    warm.automation.clear();
    warm.events.clear();
    const auto warmInput = channels ? test::silence(channels, 512) : test::Signal{};
    o.activate = false;
    return tools::fastestPrepared(kRuns, [&] {
        engine = std::make_unique<Engine>();
        test::render(warmInput, warm, engine.get());  // activation and first block, untimed
    }, [&] {
        test::render(input, o, engine.get());
    });
}

using Costs = std::map<std::string, double>;

std::map<std::string, Costs> readBaseline(const char* path) {
    std::map<std::string, Costs> all;
    std::ifstream f(path);
    std::string current;
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("machine ", 0) == 0) {
            current = trim(line.substr(8));
            continue;
        }
        std::istringstream row(line);
        std::string name;
        double cost = 0.0;
        if (!current.empty() && row >> name >> cost) all[current][name] = cost;
    }
    return all;
}

bool writeBaseline(const char* path, const std::map<std::string, Costs>& all) {
    std::ofstream f(path);
    f << "# What this product costs, per machine, in units of the benchmark's reference\n"
         "# workload (framework/tools/benchmark.cpp). Written by `benchmark --update`.\n";
    for (const auto& [name, costs] : all) {
        f << "machine " << name << "\n";
        for (const auto& [scenario, cost] : costs) {
            char value[32];
            std::snprintf(value, sizeof(value), "%.3f", cost);
            f << "  " << scenario << " " << value << "\n";
        }
    }
    return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && !std::strcmp(argv[1], "--update");
    const std::string id = machine();
    std::map<std::string, Costs> baseline = readBaseline(SUPPLE_BENCHMARK_FILE);
    const auto known = baseline.find(id);
    const Scenario scenarios[] = {{"steady", 512, false}, {"small", 32, false}, {"automated", 512, true}};

    const double ref = reference();
    std::printf("%s on %s\nreference: %.2f%% of a core\n\n", product::info::kName, id.c_str(), 100.0 * ref / 4.0);
    std::printf("  %-10s %10s %8s %10s %7s\n", "scenario", "refs", "core", "baseline", "ratio");
    Costs costs;
    bool regressed = false;
    for (const Scenario& s : scenarios) {
        const double seconds = measure(s);
        const double cost = seconds / ref;
        costs[s.name] = cost;
        std::printf("  %-10s %10.3f %7.2f%%", s.name, cost, 100.0 * seconds / 4.0);
        if (known != baseline.end() && known->second.count(s.name)) {
            const double was = known->second.at(s.name), ratio = cost / was;
            const bool slower = cost > std::max(was * kTolerance, was + kFloor);
            std::printf(" %10.3f %7.2f%s\n", was, ratio, slower ? "  SLOWER" : "");
            regressed = regressed || slower;
        } else {
            std::printf(" %10s\n", "-");
        }
    }
    if (update) {
        baseline[id] = costs;
        if (!writeBaseline(SUPPLE_BENCHMARK_FILE, baseline)) {
            std::fprintf(stderr, "could not write %s\n", SUPPLE_BENCHMARK_FILE);
            return 1;
        }
        std::printf("\nrecorded in %s\n", SUPPLE_BENCHMARK_FILE);
        return 0;
    }
    if (known == baseline.end()) {
        std::printf("\nno baseline for this machine; `benchmark --update` records one\n");
        return 77;
    }
    if (regressed) {
        std::printf("\nmore than %.1fx its baseline: a regression, or `benchmark --update` if it is meant\n", kTolerance);
        return 1;
    }
    return 0;
}
