// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Optional benchmark instrumentation. Never enabled by the application.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "supple/base/rt.h"

namespace supple::engine {

// Single producer per series; allocate before start, inspect only after stop.
// Overflow drops newest samples and is reported, never reallocates. The probe
// must outlive both the engine and backend. One measurement interval per probe.
struct BenchProbe {
    struct Sample {
        double microseconds = 0;
        std::uint32_t frames = 0;
    };
    struct Series {
        explicit Series(std::size_t capacity) : storage(capacity) {}
        void append(double us, std::uint32_t frames) SUPPLE_RT {
            if (used < storage.size())
                storage[used++] = {us, frames};
            else
                ++dropped;
        }
        std::span<const Sample> samples() const { return {storage.data(), used}; }
        std::vector<Sample> storage;
        std::size_t used = 0;
        std::uint64_t dropped = 0;
    };
    explicit BenchProbe(std::size_t capacity) : callback(capacity), renderJob(capacity) {}
    std::atomic<bool> recording{false};
    Series callback;   // backend thread, one sample per complete device period
    Series renderJob;  // renderer thread, one processRendererBlock call
    std::int64_t aheadLowWater = std::numeric_limits<std::int64_t>::max();
    std::uint64_t aheadObservations = 0;

    // Callback only, before consumption, after priming in the current epoch.
    void observeAhead(std::int64_t frames) SUPPLE_RT {
        if (recording.load(std::memory_order_relaxed)) {
            aheadLowWater = std::min(aheadLowWater, std::max<std::int64_t>(0, frames));
            ++aheadObservations;
        }
    }

    class Timer {
      public:
        Timer(BenchProbe* probe, bool callback, std::uint32_t frames) SUPPLE_RT
            : series_(probe && frames && probe->recording.load(std::memory_order_relaxed)
                          ? (callback ? &probe->callback : &probe->renderJob)
                          : nullptr),
              frames_(frames) {
            if (series_) start_ = Clock::now();
        }
        ~Timer() SUPPLE_RT {
            if (series_)
                series_->append(
                    std::chrono::duration<double, std::micro>(Clock::now() - start_).count(),
                    frames_);
        }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

      private:
        using Clock = std::chrono::steady_clock;
        Series* series_;
        std::uint32_t frames_;
        Clock::time_point start_{};
    };
};
}  // namespace supple::engine
