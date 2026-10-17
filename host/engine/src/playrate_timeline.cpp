// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "playrate_timeline.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace supple::engine::detail {

PlayrateTimeline::PlayrateTimeline(std::size_t capacity, double rate, std::vector<RatePoint> points,
                                   double origin)
    : positions_(std::bit_ceil(std::max<std::size_t>(capacity, 128))),
      checkpoints_(positions_.size() / 64),
      points_(std::move(points)),
      mask_(positions_.size() - 1),
      scalar_(rate),
      origin_(origin) {
    for (auto& point : points_) point.sample += origin_;
    reset(0, 0);
}

double PlayrateTimeline::wrapped(double phase) const SUPPLE_RT {
    if (!looping_ || phase < loopEnd_) return phase;
    return loopStart_ + std::fmod(phase - loopStart_, loopEnd_ - loopStart_);
}

double PlayrateTimeline::valueAt(const void* context, double sample) noexcept {
    const auto& self = *static_cast<const PlayrateTimeline*>(context);
    const auto& points = sample < self.curveSwitchPhase_ ? self.previousPoints_ : self.points_;
    if (points.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (self.looping_) sample = self.wrapped(sample - self.origin_) + self.origin_;
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
    previousPoints_.clear();
    curveSwitchPhase_ = -std::numeric_limits<double>::infinity();
    looping_ = loopRequested_ && loopEnd_ > loopStart_ && timeline < loopEnd_;
    clock_.reset(timeline + origin_, scalar_);
    clock_.setEnvelope(points_.empty() && previousPoints_.empty() ? nullptr : valueAt, this);
    anchor_ = device;
    anchorClock_ = clock_;
    const double initial = clock_.rate();
    // Before the new epoch no signal history exists. Linear negative preroll
    // supplies deterministic source/filter context without advancing the epoch.
    first_.store(device - static_cast<std::int64_t>(positions_.size() / 2),
                 std::memory_order_release);
    next_ = device;
    for (auto at = first_.load(std::memory_order_relaxed); at <= device; ++at)
        positions_[static_cast<std::size_t>(at) & mask_].set(
            at, (timeline + origin_) + double(at - device) * initial, initial);
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
                             std::uint32_t rampFrames, std::uint32_t waitFrames, bool changed) {
    const auto past =
        static_cast<std::int64_t>(std::min(positions_.size(), previous.positions_.size()) / 2);
    first_.store(std::max(previous.first_.load(std::memory_order_acquire), device - past),
                 std::memory_order_release);
    next_ = device;
    for (auto at = first_.load(std::memory_order_relaxed); at <= device; ++at)
        positions_[static_cast<std::size_t>(at) & mask_].set(at, previous.absolutePhase(at),
                                                             previous.rate(at));
    clock_ = previous.clockAt(device);
    if (changed) {
        previousPoints_ = previous.points_;
        curveSwitchPhase_ = previous.absolutePhase(device + waitFrames);
        const double from = previous.rate(device + waitFrames);
        clock_.transition(scalar_, from, rampFrames, waitFrames);
    } else {
        inheritControls(previous);
    }
    clock_.setEnvelope(points_.empty() && previousPoints_.empty() ? nullptr : valueAt, this);
    anchor_ = device;
    anchorClock_ = clock_;
    looping_ = previous.looping_;
    loopRequested_ = previous.loopRequested_;
    loopStart_ = previous.loopStart_;
    loopEnd_ = previous.loopEnd_;
}

bool PlayrateTimeline::hasNonUnityCurveHistory() const {
    const auto different = [](const RatePoint& point) { return point.value != 1; };
    return std::any_of(points_.begin(), points_.end(), different) ||
           std::any_of(previousPoints_.begin(), previousPoints_.end(), different);
}

void PlayrateTimeline::inheritControls(const PlayrateTimeline& previous) {
    previousPoints_ = previous.previousPoints_;
    curveSwitchPhase_ = previous.curveSwitchPhase_;
    clock_.setEnvelope(points_.empty() && previousPoints_.empty() ? nullptr : valueAt, this);
}

PlayrateTimeline::Snapshot PlayrateTimeline::snapshot(std::int64_t device) const SUPPLE_RT {
    return {clockAt(device).state(), device, loopStart_, loopEnd_, looping_};
}
void PlayrateTimeline::seed(const Snapshot& state, std::int64_t offset) SUPPLE_RT {
    loop(state.loopStart, state.loopEnd, state.looping);
    reset(state.device + offset, state.clock.timeline - origin_);
    clock_.restore(state.clock);
    clock_.setEnvelope(points_.empty() && previousPoints_.empty() ? nullptr : valueAt, this);
    anchorClock_ = clock_;
    looping_ = state.looping;
    loopRequested_ = state.looping;
}
bool PlayrateTimeline::hasHistory(std::int64_t first, std::int64_t last) const SUPPLE_RT {
    if (first < first_.load(std::memory_order_acquire)) return false;
    for (auto frame = first; frame <= last; ++frame) {
        double value, ratio;
        if (!positions_[static_cast<std::size_t>(frame) & mask_].get(frame, value, ratio))
            return false;
    }
    return true;
}

bool PlayrateTimeline::copyPast(const PlayrateTimeline& previous, std::int64_t first,
                                std::int64_t last) SUPPLE_RT {
    const auto limit =
        std::max(previous.first_.load(std::memory_order_acquire),
                 last - static_cast<std::int64_t>(std::min(capacity(), previous.capacity()) / 2));
    auto copied = last + 1;
    for (auto frame = last; frame >= limit; --frame) {
        double value, ratio;
        if (!previous.positions_[static_cast<std::size_t>(frame) & previous.mask_].get(frame, value,
                                                                                       ratio))
            break;
        positions_[static_cast<std::size_t>(frame) & mask_].set(frame, value, ratio);
        copied = frame;
    }
    // Do not pass the seed's synthetic negative preroll off as retained history.
    first_.store(copied, std::memory_order_release);
    return copied <= first;
}

void PlayrateTimeline::loop(double start, double end, bool on) SUPPLE_RT {
    if (loopStart_ == start && loopEnd_ == end && loopRequested_ == on) return;
    loopRequested_ = on;
    looping_ = on && end > start && anchorClock_.timeline() - origin_ < end;
    loopStart_ = start;
    loopEnd_ = end;
}

void PlayrateTimeline::ensure(std::int64_t last) SUPPLE_RT {
    while (next_ <= last) {
        ScalarRateEdit edit;
        while (scalarEdits.peek(edit) && edit.device <= next_ &&
               (!scalarPublication_ ||
                edit.sequence <= scalarPublication_->load(std::memory_order_acquire))) {
            if (edit.device < next_) {
                scalarLate.store(true, std::memory_order_release);
                if ((next_ & 63) != 0) break;
            }
            scalar_ = edit.rate;
            clock_.setRate(edit.rate, edit.rampFrames);
            scalarEdits.consume();
        }
        const auto index = static_cast<std::size_t>(next_) & mask_;
        if ((index & 63) == 0) checkpoints_[index / 64] = clock_;
        positions_[index].set(next_, clock_.timeline(), clock_.rate());
        std::array<double, 2> step;
        clock_.advance(step);
        ++next_;
    }
    frontier_.store(next_, std::memory_order_release);
    first_.store(std::max(first_.load(std::memory_order_relaxed),
                          next_ - static_cast<std::int64_t>(positions_.size())),
                 std::memory_order_release);
}

double PlayrateTimeline::absolutePhase(std::int64_t device) const SUPPLE_RT {
    return positions_[static_cast<std::size_t>(device) & mask_].phase.load(
        std::memory_order_relaxed);
}
double PlayrateTimeline::phase(std::int64_t device) const SUPPLE_RT {
    return absolutePhase(device) - origin_;
}
double PlayrateTimeline::absolutePosition(std::int64_t device) const SUPPLE_RT {
    return looping_ ? wrapped(phase(device)) + origin_ : absolutePhase(device);
}
double PlayrateTimeline::position(std::int64_t device) const SUPPLE_RT {
    return absolutePosition(device) - origin_;
}
double PlayrateTimeline::rate(std::int64_t device) const SUPPLE_RT {
    return positions_[static_cast<std::size_t>(device) & mask_].rate.load(
        std::memory_order_relaxed);
}
std::uint32_t PlayrateTimeline::untilWrap(std::int64_t device,
                                          std::uint32_t frames) const SUPPLE_RT {
    if (!looping_ || frames == 0) return frames;
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
