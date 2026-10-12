// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <chrono>
#include <limits>

namespace supple::tools {

// Each trial starts fresh, but construction and warm-up are outside the
// clock. A clock parameter lets tests verify the boundary without sleeping.
template <class Clock = std::chrono::steady_clock, class Prepare, class Run>
double fastestPrepared(int trials, Prepare&& prepare, Run&& run) {
    double best = std::numeric_limits<double>::infinity();
    for (int k = 0; k < trials; ++k) {
        prepare();
        const auto start = Clock::now();
        run();
        best = std::min(best, std::chrono::duration<double>(Clock::now() - start).count());
    }
    return best;
}

}  // namespace supple::tools
