// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Reading between samples: interpolators, and a delay line whose delay may
// be fractional and move every sample -- chorus, flanger, vibrato, pitch
// shifting, Doppler.
//
//   prepare:  delay_.prepare(static_cast<int>(0.05 * sampleRate) + 4);
//   process:  y = delay_.process(x, delaySamples);            // Hermite
//             y = delay_.process(x, delaySamples, dsp::Interpolation::Linear);
//
// Linear is cheapest and dulls the highs a little at fractional delays;
// Hermite (four points, third order) is the usual choice for modulated
// delays; Lagrange (four points) is flatter in level and less smooth.
// AllpassDelay passes every frequency at full level, for a fixed delay or a
// slowly moving one (a tuned string or comb), but smears fast modulation.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace supple::dsp {

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Between x0 and x1, at t in [0, 1), from the samples either side.
inline double hermite(double xm1, double x0, double x1, double x2, double t) {
    const double c1 = 0.5 * (x1 - xm1);
    const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
    const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
inline double lagrange(double xm1, double x0, double x1, double x2, double t) {
    const double a = t + 1.0, b = t, c = t - 1.0, d = t - 2.0;
    return -xm1 * b * c * d / 6.0 + x0 * a * c * d / 2.0 - x1 * a * b * d / 2.0 + x2 * a * b * c / 6.0;
}

enum class Interpolation { Linear, Hermite, Lagrange };

// A delay line read at any delay from 0 to capacity(), fractional.
class FractionalDelay {
  public:
    // Allocates.
    void prepare(int maxDelay) {
        size_t size = 4;
        while (size < static_cast<size_t>(std::max(0, maxDelay)) + 4) size <<= 1;
        buffer_.assign(size, 0.0);
        mask_ = size - 1;
        write_ = 0;
    }
    void clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0); }
    double capacity() const { return static_cast<double>(mask_ - 3); }

    void push(double x) {
        write_ = (write_ + 1) & mask_;
        buffer_[write_] = x;
    }
    // `delay` samples before the newest pushed: 0 is the newest.
    double read(double delay, Interpolation mode = Interpolation::Hermite) const {
        delay = std::clamp(delay, 0.0, capacity());
        const double whole = std::floor(delay);
        const double t = delay - whole;
        const size_t i = (write_ - static_cast<size_t>(whole)) & mask_;
        const double x0 = buffer_[i], x1 = buffer_[(i - 1) & mask_];
        if (mode == Interpolation::Linear) return lerp(x0, x1, t);
        // Reading backwards in time: newer sample on the left.
        const double xm1 = buffer_[(i + 1) & mask_], x2 = buffer_[(i - 2) & mask_];
        // At delay < 1 the "newer" neighbour is not yet written; use x0.
        const double newer = whole < 1.0 ? x0 : xm1;
        return mode == Interpolation::Lagrange ? lagrange(newer, x0, x1, x2, t) : hermite(newer, x0, x1, x2, t);
    }
    // Writes x, then reads `delay` samples back: delay 0 gives x.
    double process(double x, double delay, Interpolation mode = Interpolation::Hermite) {
        push(x);
        return read(delay, mode);
    }

  private:
    std::vector<double> buffer_ = std::vector<double>(4, 0.0);
    size_t mask_ = 3, write_ = 0;
};

// A whole-sample delay line with a first-order all-pass (Thiran) for the
// fraction: flat in level at every frequency. Delays from 1 sample up.
class AllpassDelay {
  public:
    void prepare(int maxDelay) {
        line_.prepare(maxDelay + 2);
        state_ = previous_ = 0.0;
    }
    void clear() {
        line_.clear();
        state_ = previous_ = 0.0;
    }
    double process(double x, double delay) {
        // The all-pass is best behaved with its fraction in [0.5, 1.5).
        delay = std::max(1.0, delay);
        const double whole = std::floor(delay - 0.5);
        const double fraction = delay - whole;
        const double a = (1.0 - fraction) / (1.0 + fraction);
        line_.push(x);
        const double in = line_.read(whole, Interpolation::Linear);
        const double y = a * (in - state_) + previous_;
        previous_ = in;
        state_ = y;
        return y;
    }

  private:
    FractionalDelay line_;
    double state_ = 0.0, previous_ = 0.0;
};

}  // namespace supple::dsp
