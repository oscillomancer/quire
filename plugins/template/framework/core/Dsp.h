// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Small DSP building blocks that nearly every product needs. All of them are
// real-time safe after prepare(): no allocation, no locks, no I/O. The rest
// of the library -- filters, dynamics, oversampling, oscillators, voices,
// convolution, files -- is in framework/dsp.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/ProcessorBase.h"
#include "dsp/Math.h"

namespace supple::dsp {

// One-pole exponential smoother. Natural for gains and cutoffs: it never
// overshoots and costs one multiply-add per sample. Snaps to the target once
// within its precision of it (1e-9 unless set), so a settled smoother costs
// nothing extra and never leaves an endless subnormal residue behind. Set the
// precision in proportion to the range it moves over: 1e-9 of a 200 ms
// range takes far longer to reach than 1e-9 of a gain.
class Smoother {
  public:
    void setTime(double milliseconds, double sampleRate) {
        const double samples = std::max(1.0, milliseconds * 0.001 * sampleRate);
        coefficient_ = std::exp(-1.0 / samples);
    }
    void set(double target, bool snap = false) {
        target_ = target;
        if (snap) current_ = target;
    }
    void reset(double value) { current_ = target_ = value; }
    void setPrecision(double epsilon) { epsilon_ = epsilon > 0.0 ? epsilon : 1e-9; }
    double next() {
        if (current_ != target_) {
            current_ = target_ + coefficient_ * (current_ - target_);
            if (std::fabs(current_ - target_) < epsilon_) current_ = target_;
        }
        return current_;
    }
    double value() const { return current_; }
    double target() const { return target_; }
    bool settled() const { return current_ == target_; }

  private:
    double current_ = 0.0;
    double target_ = 0.0;
    double coefficient_ = 0.0;
    double epsilon_ = 1e-9;
};

// Linear ramp of fixed duration. Use it for crossfades, where the fade must
// finish exactly rather than approach asymptotically.
class Ramp {
  public:
    void setTime(double milliseconds, double sampleRate) {
        length_ = std::max(1, static_cast<int>(std::lround(milliseconds * 0.001 * sampleRate)));
    }
    void set(double target, bool snap = false) {
        if (snap) {
            value_ = target_ = target;
            remaining_ = 0;
            return;
        }
        if (target == target_) return;
        target_ = target;
        remaining_ = length_;
        step_ = (target_ - value_) / remaining_;
    }
    double next() {
        if (remaining_ > 0) {
            value_ += step_;
            if (--remaining_ == 0) value_ = target_;
        }
        return value_;
    }
    double value() const { return value_; }
    double target() const { return target_; }
    bool settled() const { return remaining_ == 0; }

  private:
    double value_ = 0.0;
    double target_ = 0.0;
    double step_ = 0.0;
    int remaining_ = 0;
    int length_ = 1;
};

// Fixed-capacity delay line. process(x, d) writes x and returns the sample
// written d calls ago; d = 0 returns x.
class DelayLine {
  public:
    void prepare(int maxDelay) {
        size_t size = 1;
        while (size < static_cast<size_t>(std::max(0, maxDelay)) + 1) size <<= 1;
        buffer_.assign(size, 0.0);
        mask_ = size - 1;
        write_ = 0;
    }
    void clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0); }
    int capacity() const { return static_cast<int>(mask_); }
    double process(double x, int delay) {
        buffer_[write_] = x;
        const double y = buffer_[(write_ - static_cast<size_t>(delay)) & mask_];
        write_ = (write_ + 1) & mask_;
        return y;
    }

  private:
    std::vector<double> buffer_{0.0};
    size_t mask_ = 0;
    size_t write_ = 0;
};

// Dry/wet blend with the dry path delayed to match the wet path's latency.
// Without the delay, any latency at all turns Mix into a comb filter.
//
//   prepare:  dryWet_.prepare(setup, maxLatencySamples());
//   process:  dryWet_.capture(block);   // before touching the audio
//             ... process block in place ...
//             dryWet_.apply(block);
class DryWet {
  public:
    void prepare(const ProcessSetup& setup, int maxLatency, double rampMs = 20.0) {
        channels_ = setup.channels;
        delays_.resize(static_cast<size_t>(channels_));
        for (auto& d : delays_) d.prepare(maxLatency);
        dry_.assign(static_cast<size_t>(channels_),
                    std::vector<double>(static_cast<size_t>(setup.maxBlockSize), 0.0));
        mix_.setTime(rampMs, setup.sampleRate);
        latency_ = 0;
    }
    void setLatency(int samples) {
        latency_ = delays_.empty() ? 0 : std::clamp(samples, 0, delays_[0].capacity());
    }
    // 0 is dry only, 1 is wet only.
    void setMix(double wet, bool snap = false) { mix_.set(std::clamp(wet, 0.0, 1.0), snap); }
    void reset() {
        for (auto& d : delays_) d.clear();
    }

    void capture(const Block& b) {
        const int n = std::min(b.frames, static_cast<int>(dry_.empty() ? 0 : dry_[0].size()));
        for (int c = 0; c < std::min(b.channels, channels_); ++c) {
            const double* in = b.channel(c);
            double* dry = dry_[static_cast<size_t>(c)].data();
            auto& delay = delays_[static_cast<size_t>(c)];
            for (int i = 0; i < n; ++i) dry[i] = delay.process(in[i], latency_);
        }
    }

    void apply(Block& b) {
        const int channels = std::min(b.channels, channels_);
        const int n = std::min(b.frames, static_cast<int>(dry_.empty() ? 0 : dry_[0].size()));
        if (mix_.settled() && mix_.value() == 1.0) return;  // wet only, exactly
        if (mix_.settled() && mix_.value() == 0.0) {        // dry only, exactly
            for (int c = 0; c < channels; ++c)
                std::copy_n(dry_[static_cast<size_t>(c)].data(), n, b.channel(c));
            return;
        }
        for (int i = 0; i < n; ++i) {
            const double m = mix_.next();
            for (int c = 0; c < channels; ++c) {
                double* out = b.channel(c);
                const double dry = dry_[static_cast<size_t>(c)][static_cast<size_t>(i)];
                out[i] = dry + (out[i] - dry) * m;
            }
        }
    }

  private:
    std::vector<DelayLine> delays_;
    std::vector<std::vector<double>> dry_;
    Ramp mix_;
    int channels_ = 0;
    int latency_ = 0;
};

// Topology-preserving-transform state-variable filter (Zavalishin, "The Art
// of VA Filter Design"; the trapezoidal form popularised by Andrew Simper).
// Stable under fast modulation, which a direct-form biquad is not.
// Coefficients are shared; keep one State per channel.
class Svf {
  public:
    struct State {
        double ic1 = 0.0;
        double ic2 = 0.0;
    };
    struct Outputs {
        double low, band, high;
    };

    void setCoefficients(double cutoffHz, double q, double sampleRate) {
        const double fc = std::clamp(cutoffHz, 1.0, 0.49 * sampleRate);
        g_ = std::tan(kPi * fc / sampleRate);
        k_ = 1.0 / std::max(q, 1e-3);
        a1_ = 1.0 / (1.0 + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }

    Outputs tick(State& s, double x) const {
        const double v3 = x - s.ic2;
        const double v1 = a1_ * s.ic1 + a2_ * v3;
        const double v2 = s.ic2 + a2_ * s.ic1 + a3_ * v3;
        s.ic1 = 2.0 * v1 - s.ic1;
        s.ic2 = 2.0 * v2 - s.ic2;
        return {v2, v1, x - k_ * v1 - v2};
    }

    // The complex response at these coefficients, for drawing curves that
    // match the audio exactly. The SVF is the bilinear transform of the
    // analog prototype with this g, so evaluating the prototype at the
    // prewarped frequency is exact rather than an approximation.
    std::complex<double> lowpassResponse(double hz, double sampleRate) const {
        const double w = prewarped(hz, sampleRate);
        return 1.0 / std::complex<double>(1.0 - w * w, k_ * w);
    }
    std::complex<double> highpassResponse(double hz, double sampleRate) const {
        const double w = prewarped(hz, sampleRate);
        return -w * w / std::complex<double>(1.0 - w * w, k_ * w);
    }

  private:
    double prewarped(double hz, double sampleRate) const {
        return std::tan(kPi * std::clamp(hz, 0.0, 0.4999 * sampleRate) / sampleRate) / g_;
    }

    double g_ = 0.0, k_ = 1.0, a1_ = 0.0, a2_ = 0.0, a3_ = 0.0;
};

}  // namespace supple::dsp
