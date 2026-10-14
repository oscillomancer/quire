// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include "supple/console9/console9.h"
using namespace supple::console9;
namespace {
template <class Stage, class T>
void bench(const char* name, bool ramp, bool linear = false) {
    constexpr uint32_t block = 512, loops = 4096;
    Stage stage;
    stage.setLinear(linear);
    stage.reset(seedsFor(123, 456));
    std::vector<T> in(block), l(block), r(block);
    for (uint32_t i = 0; i < block; ++i) in[i] = T(.1 * std::sin(double(i) * .1));
    const T* inputs[]{in.data(), in.data()};
    T* outputs[]{l.data(), r.data()};
    for (int i = 0; i < 32; ++i) stage.process(inputs, outputs, block);
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < loops; ++i) {
        if (ramp) stage.set(i % 2 ? .3 : 2., i % 2 ? .1 : .9, block);
        stage.process(inputs, outputs, block);
    }
    const auto end = std::chrono::steady_clock::now();
    const double ns =
        std::chrono::duration<double, std::nano>(end - start).count() / (double(loops) * block);
    std::printf("%-7s %2zu %-6s %-8s %.3f ns/stereo-frame %.4f%% core@48k checksum=%.9g\n", name,
                sizeof(T) * 8, linear ? "linear" : "curves", ramp ? "ramping" : "constant", ns,
                ns * 48000 / 1e7, double(l[17]) + double(r[31]));
}
}  // namespace
int main() {
    bench<Channel, float>("channel", false);
    bench<Channel, float>("channel", true);
    bench<Buss, float>("buss", false);
    bench<Buss, float>("buss", true);
    bench<Channel, double>("channel", false);
    bench<Channel, double>("channel", true);
    bench<Buss, double>("buss", false);
    bench<Buss, double>("buss", true);
    bench<Channel, float>("channel", false, true);
    bench<Channel, float>("channel", true, true);
    bench<Buss, float>("buss", false, true);
    bench<Buss, float>("buss", true, true);
    bench<Channel, double>("channel", false, true);
    bench<Channel, double>("channel", true, true);
    bench<Buss, double>("buss", false, true);
    bench<Buss, double>("buss", true, true);
}
