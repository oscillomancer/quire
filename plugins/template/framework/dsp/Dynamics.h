// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parts of a compressor, limiter or gate: level detection (peak and RMS
// followers, true peak), the static curve (GainComputer), and lookahead
// (SlidingMax, Lookahead). A compressor is these in a row:
//
//   level = follower.tick(state, sidechain);                // linear
//   gain  = dbToGain(computer.gainDb(gainToDb(level)));     // <= 1
//   out   = in * gain;
//
// prepare() allocates where noted; tick() never does.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "dsp/Math.h"
#include "dsp/Fir.h"

namespace supple::dsp {

// The coefficient that brings a one-pole within 1/e of its target in `ms`.
inline double timeCoefficient(double ms, double sampleRate) {
    const double samples = ms * 0.001 * sampleRate;
    return samples <= 0.0 ? 0.0 : std::exp(-1.0 / samples);
}

// The rectified signal through a one-pole that rises at the attack time and
// falls at the release time: the usual compressor detector.
class EnvelopeFollower {
  public:
    struct State {
        double envelope = 0.0;
    };
    void setTimes(double attackMs, double releaseMs, double sampleRate) {
        attack_ = timeCoefficient(attackMs, sampleRate);
        release_ = timeCoefficient(releaseMs, sampleRate);
    }
    double tick(State& s, double x) const {
        const double level = std::fabs(x);
        const double c = level > s.envelope ? attack_ : release_;
        s.envelope = level + c * (s.envelope - level);
        return s.envelope;
    }

  private:
    double attack_ = 0.0, release_ = 0.0;
};

// The root of an exponentially weighted mean square, `ms` its time constant:
// a sine of peak 1 reads 0.707 once settled.
class RmsFollower {
  public:
    struct State {
        double meanSquare = 0.0;
    };
    void setTime(double ms, double sampleRate) { c_ = timeCoefficient(ms, sampleRate); }
    double tick(State& s, double x) const {
        s.meanSquare = x * x + c_ * (s.meanSquare - x * x);
        return std::sqrt(s.meanSquare);
    }

  private:
    double c_ = 0.0;
};

// The static curve of a downward compressor: how many dB to turn a level
// down by. Below the knee nothing; above it, `ratio` dB in for each dB out;
// across the knee, a quadratic joining the two. An infinite ratio is a
// limiter.
struct GainComputer {
    double thresholdDb = -20.0;
    double ratio = 4.0;  // >= 1; std::numeric_limits<double>::infinity() for a limiter
    double kneeDb = 6.0;

    // The gain in dB, <= 0, for a detected level in dB.
    double gainDb(double levelDb) const {
        const double slope = 1.0 / std::max(ratio, 1.0) - 1.0;  // <= 0
        const double over = levelDb - thresholdDb;
        if (kneeDb > 0.0 && std::fabs(over) <= 0.5 * kneeDb) {
            const double into = over + 0.5 * kneeDb;
            return slope * into * into / (2.0 * kneeDb);
        }
        return over > 0.0 ? slope * over : 0.0;
    }
};

// The largest of the last `window` values, in amortised constant time (a
// monotonic queue over a fixed buffer). For the smallest, push negated
// values and negate the result.
class SlidingMax {
  public:
    // Allocates.
    void prepare(int maxWindow) {
        capacity_ = static_cast<size_t>(std::max(1, maxWindow)) + 1;
        entries_.assign(capacity_, Entry{});
        setWindow(maxWindow);
        clear();
    }
    void setWindow(int window) { window_ = static_cast<uint64_t>(std::clamp(window, 1, static_cast<int>(capacity_) - 1)); }
    int window() const { return static_cast<int>(window_); }
    void clear() {
        head_ = tail_ = 0;
        time_ = 0;
    }

    double push(double x) {
        // Anything no larger than x can never be the maximum again.
        while (tail_ != head_ && back().value <= x) --tail_;
        entries_[tail_++ % capacity_] = {x, time_};
        while (entries_[head_ % capacity_].time + window_ <= time_) ++head_;
        ++time_;
        return entries_[head_ % capacity_].value;
    }

  private:
    struct Entry {
        double value = 0.0;
        uint64_t time = 0;
    };
    Entry& back() { return entries_[(tail_ - 1) % capacity_]; }

    std::vector<Entry> entries_{Entry{}, Entry{}};
    size_t capacity_ = 2;
    uint64_t head_ = 0, tail_ = 0, time_ = 0, window_ = 1;
};

// Lookahead for a limiter: give it, each sample, the gain that sample needs
// (<= 1), and it returns a smooth gain for the audio delayed by length()
// samples, which is never above what any sample needs when it plays. It
// ramps down over `length` samples ahead of a peak (the minimum over the
// window, then a moving average of the same length) and releases the same
// way; follow it with an EnvelopeFollower-style release if a slower one is
// wanted. Report length() as latency, and delay the audio by it.
class Lookahead {
  public:
    // Allocates.
    void prepare(int maxLength) {
        minimum_.prepare(std::max(1, maxLength) + 1);
        average_.assign(static_cast<size_t>(std::max(1, maxLength)), 1.0);
        setLength(maxLength);
    }
    void setLength(int samples) {
        length_ = std::clamp(samples, 1, static_cast<int>(average_.size()));
        minimum_.setWindow(length_ + 1);
        clear();
    }
    int length() const { return length_; }
    void clear() {
        minimum_.clear();
        std::fill(average_.begin(), average_.end(), 1.0);
        sum_ = static_cast<double>(length_);
        write_ = 0;
        count_ = 0;
    }

    double tick(double neededGain) {
        const double m = -minimum_.push(-neededGain);
        double& slot = average_[static_cast<size_t>(write_)];
        sum_ += m - slot;
        slot = m;
        write_ = write_ + 1 == length_ ? 0 : write_ + 1;
        // Re-add from scratch now and then, so rounding never accumulates.
        if (++count_ == 4096) {
            count_ = 0;
            sum_ = 0.0;
            for (int k = 0; k < length_; ++k) sum_ += average_[static_cast<size_t>(k)];
        }
        return std::min(1.0, sum_ / length_);
    }

  private:
    SlidingMax minimum_;
    std::vector<double> average_{1.0};
    double sum_ = 1.0;
    int length_ = 1, write_ = 0, count_ = 0;
};

// The peak between samples as well as on them (ITU-R BS.1770 Annex 2): the
// signal interpolated four times over, its largest magnitude reported. A
// full-scale sine can read +3 dB over its sample peak at a quarter of the
// sample rate.
class TruePeak {
  public:
    static constexpr int kFactor = 4, kTaps = 40;  // per phase
    struct State {
        // Each sample is kept twice, kTaps apart, so the newest kTaps are
        // always contiguous: no wrapping index per tap.
        double history[2 * kTaps] = {};
        int write = 0;
    };

    // Designs the interpolator once, allocating at construction, off the audio thread.
    TruePeak() {
        const std::vector<double> h = lowpassKernel(kFactor * kTaps, 0.5 / kFactor, 70.0);
        for (int p = 0; p < kFactor; ++p)
            for (int k = 0; k < kTaps; ++k) phase_[p][k] = kFactor * h[static_cast<size_t>(k * kFactor + p)];
    }

    // The largest magnitude among the four interpolated points this sample
    // adds, or the current sample peak, whichever is larger. The interpolator
    // has 19.875 input samples of delay (160 taps at 4x); the sample floor
    // is immediate. There is no delay imposed on the audio signal.
    double tick(State& s, double x) const {
        s.history[s.write] = s.history[s.write + kTaps] = x;
        const double* newest = s.history + s.write + kTaps;
        s.write = s.write + 1 == kTaps ? 0 : s.write + 1;
        double peak = std::fabs(x);
        for (int p = 0; p < kFactor; ++p) {
            double y = 0.0;
            for (int k = 0; k < kTaps; ++k) y += phase_[p][k] * newest[-k];
            peak = std::max(peak, std::fabs(y));
        }
        return peak;
    }

  private:
    double phase_[kFactor][kTaps] = {};
};

}  // namespace supple::dsp
