// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <array>
#include "eq/processor.h"
int main() {
    supple::eq::Processor p;
    p.prepare(48000, 64);
    std::array<float, 64> x{}, y{}, z{};
    const float* in[] = {x.data(), x.data()};
    float* out[] = {y.data(), z.data()};
    p.process(in, out, 64);
    for (auto v : y)
        if (v != 0) return 1;
    return 0;
}
