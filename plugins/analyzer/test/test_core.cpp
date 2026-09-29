// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <analyzer/Processor.h>

using analyzer::Processor;
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("  %-56s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

template <typename T>
std::vector<T> render(int block, double gainDb) {
    constexpr int count = 8192;
    Processor processor;
    processor.prepare(48000.0, 1);
    Processor::Params params;
    params.inputGainDb = gainDb;
    processor.setParams(params);
    std::vector<T> audio(count);
    for (int i = 0; i < count; ++i) audio[i] = static_cast<T>(0.1 * std::sin(i * 0.037));
    for (int offset = 0; offset < count; offset += block) {
        const int frames = std::min(block, count - offset);
        T* pointer = audio.data() + offset;
        processor.process(&pointer, frames);
    }
    return audio;
}

int main() {
    const auto a = render<double>(1, 6.0);
    const auto b = render<double>(1024, 6.0);
    double error = 0.0;
    for (size_t i = 0; i < a.size(); ++i) error = std::max(error, std::abs(a[i] - b[i]));
    check(error == 0.0, "DSP output is host-block-size invariant");

    const double expected = 0.1 * std::sin(100 * 0.037) * analyzer::dbToGain(6.0);
    check(std::abs(a[100] - expected) < 1.0e-12,
          "restored initial gain snaps without a startup ramp");

    const auto f = render<float>(257, 6.0);
    double precision = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        precision = std::max(precision, std::abs(a[i] - static_cast<double>(f[i])));
    check(precision < 2.0e-7, "float and double paths agree within float precision");

    Processor bypass;
    bypass.prepare(48000.0, 1);
    Processor::Params params; params.inputGainDb = 18.0; params.bypass = true;
    bypass.setParams(params);
    double sample = 0.25; double* pointer = &sample; bypass.process(&pointer, 1);
    check(sample == 0.25, "initial bypass is exactly dry");
    check(std::isfinite(sample), "processing produces finite output");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
