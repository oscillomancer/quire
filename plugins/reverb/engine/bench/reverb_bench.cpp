// SPDX-License-Identifier: MIT
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "reverb/processor.h"
int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 120;
    if (seconds <= 0) return 2;
    std::array<double, 512> l{}, r{}, ol{}, orr{};
    for (std::size_t i = 0; i < 512; ++i) {
        l[i] = .1 * std::sin(static_cast<double>(i) * .041);
        r[i] = .1 * std::cos(static_cast<double>(i) * .071);
    }
    const double* in[] = {l.data(), r.data()};
    double* out[] = {ol.data(), orr.data()};
    const auto blocks = static_cast<std::size_t>(std::ceil(seconds * 48000 / 512));
    for (int scenario : {0, 1, 2}) {
        std::array<double, 5> trials{};
        for (auto& trial : trials) {
            supple::reverb::Processor p;
            if (!p.prepare(48000, 512)) return 1;
            if (scenario == 1) {
                p.setParam(supple::reverb::kSize, 85);
                p.setParam(supple::reverb::kDecay, 4500);
                p.setParam(supple::reverb::kPredelay, 25);
                p.setParam(supple::reverb::kHighDecay, .4);
            }
            if (scenario == 2) {
                p.setParam(supple::reverb::kSize, 100);
                p.setParam(supple::reverb::kDecay, 60000);
                p.setParam(supple::reverb::kLowDecay, 4);
                p.setParam(supple::reverb::kHighDecay, .1);
                p.setParam(supple::reverb::kDiffusion, 100);
                p.setParam(supple::reverb::kModulation, 100);
                p.setParam(supple::reverb::kWidth, 200);
                p.setParam(supple::reverb::kMix, 100);
            }
            for (int i = 0; i < 128; ++i) p.process(in, out, 512);
            const auto start = std::clock();
            for (std::size_t i = 0; i < blocks; ++i) p.process(in, out, 512);
            trial = 100 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC /
                    (static_cast<double>(blocks) * 512 / 48000);
        }
        std::sort(trials.begin(), trials.end());
        std::printf("%s: %.4f %% of one core, 48 kHz, stereo512, median5\n",
                    scenario == 0   ? "default"
                    : scenario == 1 ? "hall"
                                    : "maximum texture",
                    trials[2]);
    }
    std::printf("checksum %.12g\n", ol[511] + orr[511]);
}
