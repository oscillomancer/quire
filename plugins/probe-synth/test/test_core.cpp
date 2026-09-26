// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <probesynth/Processor.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
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
