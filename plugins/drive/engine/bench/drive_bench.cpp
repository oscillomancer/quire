// SPDX-License-Identifier: MIT
// Opt-in CPU benchmark: -O2, stereo 48 kHz, 256 frames, median of three runs.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <numbers>
#include <vector>
#include "drive/processor.h"
using namespace supple::drive;
int main() {
    constexpr int frames = 256, blocks = 1875;  // Ten seconds per repetition.
    std::vector<float> left(frames * blocks), right(left.size());
    for (size_t n = 0; n < left.size(); ++n) {
        const double phase = 2 * std::numbers::pi * 1000 * n / 48000;
        left[n] = static_cast<float>(std::sqrt(2.) * std::pow(10., -.9) * std::sin(phase));
        right[n] = static_cast<float>(std::sqrt(2.) * std::pow(10., -.9) * std::cos(phase));
    }
    std::printf(
        "-O2, stereo 48 kHz/256, 4x, -18 dBFS RMS 1 kHz sine, auto gain on, "
        "bias/tone 0, wet 100%%; median of three 10 s runs, process CPU time\n");
    double checksum = 0;
    for (int variant = -1; variant < 6; ++variant) {
        const int curve = std::max(0, variant);
        Processor p;
        p.setParam(kCurve, curve);
        p.setParam(kDrive, variant < 0 ? 6 : 24);
        p.prepare(48000, frames, 2);
        float l[frames]{}, r[frames]{};
        float* out[] = {l, r};
        std::array<double, 3> runs{};
        for (auto& run : runs) {
            p.reset();
            const float* warm[] = {left.data(), right.data()};
            for (int i = 0; i < 32; ++i) p.process(warm, out, frames);
            const auto begin = std::clock();
            for (int b = 0; b < blocks; ++b) {
                const float* in[] = {left.data() + b * frames, right.data() + b * frames};
                p.process(in, out, frames);
            }
            run = 100 * static_cast<double>(std::clock() - begin) / CLOCKS_PER_SEC / 10;
            checksum += static_cast<double>(l[0]) + static_cast<double>(r[0]);
        }
        std::sort(runs.begin(), runs.end());
        std::printf("%-10s drive=%2d dB: %.3f%% of one core, %.1f ns/stereo frame\n",
                    kCurveChoices[curve].key.data(), variant < 0 ? 6 : 24, runs[1],
                    runs[1] * 1e7 / 48000);
    }
    return std::isfinite(checksum) ? 0 : 1;
}
