// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include "playrate_dsp.h"
#include "supple/model/env_shape.h"

namespace supple::engine::detail {

struct RatePoint {
    double sample, value;
    model::PointShape shape;
    double tension;
};

// One executor owns advancement; its node jobs only read the prepared window.
// PDC is applied in device frames BEFORE looking up timeline coordinates.
// Unwrapped phase is monotonic through project loops, as is device/stream time.
class PlayrateTimeline {
  public:
    PlayrateTimeline(std::size_t capacity, double rate, std::vector<RatePoint> points);
    void reset(std::int64_t device, double timeline) SUPPLE_RT;
    void adopt(const PlayrateTimeline& previous, std::int64_t device,
               std::uint32_t rampFrames) SUPPLE_RT;
    void loop(double start, double end, bool on) SUPPLE_RT;
    void ensure(std::int64_t last) SUPPLE_RT;
    double phase(std::int64_t device) const SUPPLE_RT;
    double position(std::int64_t device) const SUPPLE_RT;
    double rate(std::int64_t device) const SUPPLE_RT;
    double wrapped(double phase) const SUPPLE_RT;
    std::uint32_t untilWrap(std::int64_t device, std::uint32_t frames) const SUPPLE_RT;
    std::int64_t nextFrame() const SUPPLE_RT { return next_; }
    std::size_t capacity() const { return positions_.size(); }

  private:
    static double valueAt(const void* context, double sample) noexcept;
    PlayrateClock clockAt(std::int64_t device) const SUPPLE_RT;
    struct Boundary {
        double phase = 0, rate = 1;
    };
    std::vector<Boundary> positions_;
    std::vector<PlayrateClock> checkpoints_;
    std::vector<RatePoint> points_;
    std::size_t mask_;
    double scalar_, loopStart_ = 0, loopEnd_ = 0;
    bool looping_ = false;
    std::int64_t first_ = 0, next_ = 0, anchor_ = 0;
    PlayrateClock clock_, anchorClock_;
};

}  // namespace supple::engine::detail
