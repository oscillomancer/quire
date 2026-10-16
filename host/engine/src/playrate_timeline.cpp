// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "playrate_timeline.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace supple::engine::detail {

PlayrateTimeline::PlayrateTimeline(std::size_t capacity, double rate, std::vector<RatePoint> points)
    : positions_(std::bit_ceil(std::max<std::size_t>(capacity, 128))),
      checkpoints_(positions_.size() / 64),
      points_(std::move(points)),
      mask_(positions_.size() - 1),
      scalar_(rate) {
    reset(0, 0);
}

double PlayrateTimeline::wrapped(double phase) const SUPPLE_RT {
    if (!looping_ || phase < loopEnd_) return phase;
    return loopStart_ + std::fmod(phase - loopStart_, loopEnd_ - loopStart_);
}

double PlayrateTimeline::valueAt(const void* context, double sample) noexcept {
    const auto& self = *static_cast<const PlayrateTimeline*>(context);
    const auto& points = self.points_;
    if (points.empty()) return self.scalar_;
    sample = self.wrapped(sample);
    const auto next = std::upper_bound(points.begin(), points.end(), sample,
                                       [](double s, const RatePoint& p) { return s < p.sample; });
    if (next == points.begin()) return points.front().value;
    if (next == points.end()) return points.back().value;
    const auto& a = *(next - 1);
    const auto& b = *next;
    if (b.sample <= a.sample) return b.value;
    return a.value +
           (b.value - a.value) *
               model::shapeCurve(a.shape, a.tension, (sample - a.sample) / (b.sample - a.sample));
}

void PlayrateTimeline::reset(std::int64_t device, double timeline) SUPPLE_RT {
    clock_.reset(timeline, scalar_);
    clock_.setEnvelope(points_.empty() ? nullptr : valueAt, this);
    anchor_ = device;
    anchorClock_ = clock_;
    const double initial = clock_.rate();
    // Before the new epoch no signal history exists. Linear negative preroll
    // supplies deterministic source/filter context without advancing the epoch.
    first_ = device - static_cast<std::int64_t>(positions_.size() / 2);
    next_ = device;
    for (auto at = first_; at <= device; ++at)
        positions_[static_cast<std::size_t>(at) & mask_] = {
            timeline + double(at - device) * initial, initial};
}

PlayrateClock PlayrateTimeline::clockAt(std::int64_t device) const SUPPLE_RT {
    const auto at = device - static_cast<std::int64_t>(static_cast<std::size_t>(device) & 63);
    const auto start = std::max(at, anchor_);
    auto copy =
        at <= anchor_ ? anchorClock_ : checkpoints_[(static_cast<std::size_t>(at) & mask_) / 64];
    std::array<double, 65> discard{};
    copy.advance(std::span(discard).first(static_cast<std::size_t>(device - start) + 1));
    return copy;
}

void PlayrateTimeline::adopt(const PlayrateTimeline& previous, std::int64_t device,
                             std::uint32_t rampFrames) SUPPLE_RT {
    const auto past =
        static_cast<std::int64_t>(std::min(positions_.size(), previous.positions_.size()) / 2);
    first_ = std::max(previous.first_, device - past);
    next_ = device;
    for (auto at = first_; at <= device; ++at)
        positions_[static_cast<std::size_t>(at) & mask_] =
            previous.positions_[static_cast<std::size_t>(at) & previous.mask_];
    clock_ = previous.clockAt(device);
    clock_.setEnvelope(points_.empty() ? nullptr : valueAt, this);
    clock_.setRate(scalar_, rampFrames);
    anchor_ = device;
    anchorClock_ = clock_;
    looping_ = previous.looping_;
    loopStart_ = previous.loopStart_;
    loopEnd_ = previous.loopEnd_;
}

void PlayrateTimeline::loop(double start, double end, bool on) SUPPLE_RT {
    looping_ = on && end > start;
    loopStart_ = start;
    loopEnd_ = end;
}

void PlayrateTimeline::ensure(std::int64_t last) SUPPLE_RT {
    while (next_ <= last) {
        const auto index = static_cast<std::size_t>(next_) & mask_;
        if ((index & 63) == 0) checkpoints_[index / 64] = clock_;
        positions_[index] = {clock_.timeline(), clock_.rate()};
        std::array<double, 2> step;
        clock_.advance(step);
        ++next_;
    }
    first_ = std::max(first_, next_ - static_cast<std::int64_t>(positions_.size()));
}

double PlayrateTimeline::phase(std::int64_t device) const SUPPLE_RT {
    return positions_[static_cast<std::size_t>(device) & mask_].phase;
}
double PlayrateTimeline::position(std::int64_t device) const SUPPLE_RT {
    return wrapped(phase(device));
}
double PlayrateTimeline::rate(std::int64_t device) const SUPPLE_RT {
    return positions_[static_cast<std::size_t>(device) & mask_].rate;
}
std::uint32_t PlayrateTimeline::untilWrap(std::int64_t device,
                                          std::uint32_t frames) const SUPPLE_RT {
    if (!looping_) return frames;
    const double here = phase(device);
    const double end = here < loopEnd_ ? loopEnd_ : here + (loopEnd_ - position(device));
    std::uint32_t lo = 1, hi = frames;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (phase(device + mid) < end)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

}  // namespace supple::engine::detail
