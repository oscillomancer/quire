// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <atomic>
#include <cmath>
#include <limits>
#include "playrate_dsp.h"
#include "supple/base/spsc.h"
#include "supple/model/env_shape.h"

namespace supple::engine::detail {

struct ScalarRateEdit {
    std::int64_t device = 0;
    double rate = 1;
    std::uint64_t sequence = 0;
    std::uint32_t rampFrames = 0;
};

struct RatePoint {
    double sample, value;
    model::PointShape shape;
    double tension;
    bool operator==(const RatePoint&) const = default;
};

// One executor owns advancement; its node jobs only read the prepared window.
// PDC is applied in device frames BEFORE looking up timeline coordinates.
// Unwrapped phase is monotonic through project loops, as is device/stream time.
class PlayrateTimeline {
  public:
    struct Snapshot {
        PlayrateClock::State clock;
        std::int64_t device = 0;
        double loopStart = 0, loopEnd = 0;
        bool looping = false;
    };
    // Control producer; advancement is the sole consumer. Publication gates
    // both executor queues so neither observes half a control transaction.
    SpscRing<ScalarRateEdit, 1024> scalarEdits;
    std::atomic<bool> scalarLate{false};
    void bindScalarPublication(const std::atomic<std::uint64_t>* publication) {
        scalarPublication_ = publication;
    }
    std::int64_t frontier() const { return frontier_.load(std::memory_order_acquire); }
    Snapshot snapshot(std::int64_t device) const SUPPLE_RT;
    void seed(const Snapshot& state, std::int64_t deviceOffset = 0) SUPPLE_RT;
    bool hasHistory(std::int64_t first, std::int64_t last) const SUPPLE_RT;
    // Retain all available past (up to half the smaller ring), requiring at
    // least [first, last]. Safe to copy from the other executor's active ring.
    bool copyPast(const PlayrateTimeline& previous, std::int64_t first,
                  std::int64_t last) SUPPLE_RT;
    const std::vector<RatePoint>& points() const { return points_; }
    bool hasNonUnityCurveHistory() const;  // immutable controls, model-thread guard
    PlayrateTimeline(std::size_t capacity, double rate, std::vector<RatePoint> points,
                     double origin = 0);
    void reset(std::int64_t device, double timeline) SUPPLE_RT;
    void adopt(const PlayrateTimeline& previous, std::int64_t device, std::uint32_t rampFrames,
               std::uint32_t waitFrames = 0, bool changed = true);
    void inheritControls(const PlayrateTimeline& previous);
    void loop(double start, double end, bool on) SUPPLE_RT;
    void ensure(std::int64_t last) SUPPLE_RT;
    double phase(std::int64_t device) const SUPPLE_RT;
    double absolutePhase(std::int64_t device) const SUPPLE_RT;
    double absolutePosition(std::int64_t device) const SUPPLE_RT;
    double origin() const { return origin_; }
    double position(std::int64_t device) const SUPPLE_RT;
    double rate(std::int64_t device) const SUPPLE_RT;
    double wrapped(double phase) const SUPPLE_RT;
    bool looping() const SUPPLE_RT { return looping_; }
    double loopStart() const SUPPLE_RT { return loopStart_; }
    double loopEnd() const SUPPLE_RT { return loopEnd_; }
    double cycleLength() const SUPPLE_RT { return loopEnd_ - loopStart_; }
    std::int64_t cycle(double phase) const SUPPLE_RT {
        return looping_ && phase >= loopEnd_
                   ? static_cast<std::int64_t>(std::floor((phase - loopStart_) / cycleLength()))
                   : 0;
    }
    std::uint32_t untilWrap(std::int64_t device, std::uint32_t frames) const SUPPLE_RT;
    std::int64_t nextFrame() const SUPPLE_RT { return next_; }
    std::size_t capacity() const { return positions_.size(); }

  private:
    static double valueAt(const void* context, double sample) noexcept;
    PlayrateClock clockAt(std::int64_t device) const SUPPLE_RT;
    struct Boundary {
        std::atomic<std::int64_t> tag{INT64_MIN};
        std::atomic<double> phase{0}, rate{1};
        void set(std::int64_t frame, double value, double ratio) SUPPLE_RT {
            tag.store(INT64_MIN, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            phase.store(value, std::memory_order_relaxed);
            rate.store(ratio, std::memory_order_relaxed);
            tag.store(frame, std::memory_order_release);
        }
        bool get(std::int64_t frame, double& value, double& ratio) const SUPPLE_RT {
            const auto before = tag.load(std::memory_order_acquire);
            value = phase.load(std::memory_order_relaxed);
            ratio = rate.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            return before == frame && before == tag.load(std::memory_order_relaxed);
        }
    };
    std::vector<Boundary> positions_;
    std::vector<PlayrateClock> checkpoints_;
    std::vector<RatePoint> points_, previousPoints_;
    double curveSwitchPhase_ = -std::numeric_limits<double>::infinity();
    std::size_t mask_;
    double scalar_, origin_ = 0, loopStart_ = 0, loopEnd_ = 0;
    bool looping_ = false, loopRequested_ = false;
    std::atomic<std::int64_t> first_{0};  // retained floor, read by control-side copying
    std::int64_t next_ = 0, anchor_ = 0;
    PlayrateClock clock_, anchorClock_;
    const std::atomic<std::uint64_t>* scalarPublication_ = nullptr;
    std::atomic<std::int64_t> frontier_{0};
};

}  // namespace supple::engine::detail
