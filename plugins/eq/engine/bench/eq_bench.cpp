// Offline CPU/accuracy measurements, never registered as a CTest timing test.
// Build with -O2: cmake --preset release -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG'
// cmake --build --preset release --target eq-bench -j6
// eq-bench [audio-seconds-per-trial] | eq-bench --accuracy
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <numbers>
#include "accuracy.h"
#include "eq/processor.h"
using namespace supple::eq;
namespace {
constexpr double kRate = 48000;
constexpr std::uint32_t kBlock = 512;
double cpu(int phase, int count, double seconds, bool autoGain) {
    auto v = defaults();
    v[kPhase] = phase;
    v[kAutoGain] = autoGain;
    constexpr int types[] = {HighPass, Bell, Bell, HighShelf, Bell, LowShelf, Notch, LowPass};
    constexpr double frequencies[] = {80, 250, 3000, 8000, 700, 120, 5000, 16000};
    constexpr double gains[] = {0, -3, 3, 3, -4, 2, 0, 0};
    for (std::size_t b = 0; b < kBands; ++b) {
        v[bandParam(b, Enabled)] = b < static_cast<std::size_t>(count) ? 1 : 0;
        v[bandParam(b, Type)] = types[b];
        v[bandParam(b, Freq)] = frequencies[b];
        v[bandParam(b, Gain)] = gains[b];
    }
    Processor p;
    for (std::size_t i = 0; i < v.size(); ++i) p.setParam(i, v[i]);
    p.prepare(kRate, kBlock);
    std::array<float, kBlock> l{}, r{}, outL{}, outR{};
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = static_cast<float>(
            .1 * std::sin(2 * std::numbers::pi * 137 * static_cast<double>(i) / kRate));
        r[i] = static_cast<float>(
            .1 * std::sin(2 * std::numbers::pi * 211 * static_cast<double>(i) / kRate));
    }
    const float* input[] = {l.data(), r.data()};
    float* output[] = {outL.data(), outR.data()};
    for (int k = 0; k < 128; ++k) p.process(input, output, kBlock);
    const auto blocks = std::max(1L, static_cast<long>(seconds * kRate / kBlock));
    volatile float sink = 0;
    const auto start = std::clock();
    for (long k = 0; k < blocks; ++k) {
        p.process(input, output, kBlock);
        sink = sink + outL[0];
    }
    const auto elapsed = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    return 100 * elapsed / (static_cast<double>(blocks) * kBlock / kRate);
}
void report(const char* name, int type, double fs, const measurement::Error& error) {
    std::printf("%-19s %-10s %5.0f Hz: %9.6f dB (f0=%9.3f gain=%5.1f Q=%5.2f probe=%9.3f Hz)\n",
                name,
                type == Bell       ? "bell"
                : type == LowShelf ? "low shelf"
                                   : "high shelf",
                fs, error.db, error.f0, error.gain, error.q, error.hz);
}
}  // namespace
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--accuracy") == 0) {
        for (double fs : {44100., 48000.})
            for (int type : {Bell, LowShelf, HighShelf}) {
                report("ordinary", type, fs, measurement::ordinary(type, fs, 2048));
                if (type != Bell)
                    report("resonant shelves", type, fs,
                           measurement::extreme(type, fs, false, 2048));
                report("near Nyquist", type, fs, measurement::extreme(type, fs, true, 2048));
            }
        return 0;
    }
    const auto seconds = argc > 1 ? std::max(1., std::atof(argv[1])) : 120.;
    std::printf(
        "-O2 recommended; 48 kHz, 512-frame stereo, median of 5 CPU-time trials (%.0f audio "
        "seconds each)\n",
        seconds);
    for (int bands : {4, 8})
        for (bool autoGain : {false, true}) {
            std::array<double, 5> minimum{}, natural{};
            for (std::size_t k = 0; k < minimum.size(); ++k) {
                minimum[k] = cpu(0, bands, seconds, autoGain);
                natural[k] = cpu(1, bands, seconds, autoGain);
            }
            std::sort(minimum.begin(), minimum.end());
            std::sort(natural.begin(), natural.end());
            std::printf("%d bands auto-gain %s: minimum %.4f%% natural %.4f%% ratio %.3fx\n", bands,
                        autoGain ? "on " : "off", minimum[2], natural[2], natural[2] / minimum[2]);
        }
}
