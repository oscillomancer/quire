// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <probesynth/Processor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../plugin/src/ProbeSynthParams.h"
using probesynth::Processor;
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}
std::vector<double> render(int block, int waveform = 0) {
    Processor p;
    p.prepare(48000, 1);
    Processor::Params params;
    params.waveform = waveform;
    p.setParams(params);
    p.noteOn(1, 0, 60, 0.8);
    std::vector<double> out(8192);
    for (int off = 0; off < 8192; off += block) {
        double* ptr = out.data() + off;
        p.process(&ptr, std::min(block, 8192 - off));
    }
    return out;
}
int main() {
    auto tableStatus = supple::params::validateTable(probesynth::kParams);
    if (!tableStatus) {
        std::printf("table error: %s %s\n", tableStatus.error().code.c_str(),
                    tableStatus.error().message.c_str());
        for (const auto& d : tableStatus.error().details)
            std::printf("  %s %s %s\n", d.code.c_str(), d.path.c_str(), d.message.c_str());
    }
    check(static_cast<bool>(tableStatus), "semantic parameter table validates");
    const auto& cutoff = probesynth::kParams[probesynth::kCutoff];
    check(supple::params::toClap(cutoff, cutoff.min) == 0.0 &&
              supple::params::toClap(cutoff, cutoff.max) == 1.0,
          "log cutoff uses normalized CLAP endpoints");
    for (int shape = 0; shape < 4; ++shape) {
        auto a = render(1, shape), b = render(257, shape);
        check(a == b, "block-size invariant waveform");
        check(std::any_of(a.begin(), a.end(), [](double x) { return std::abs(x) > 1e-6; }),
              "note generates audio");
        check(std::all_of(a.begin(), a.end(), [](double x) { return std::isfinite(x); }),
              "finite audio");
    }
    Processor p;
    p.prepare(48000, 1);
    p.noteOn(1, 0, 60, 1);
    p.noteChoke(1, 0, 60);
    double x = 1;
    double* ptr = &x;
    p.process(&ptr, 1);
    check(x == 0, "choke silences voice");
    return failures ? 1 : 0;
}
