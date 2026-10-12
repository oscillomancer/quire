// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Filters: the cookbook biquads, Butterworth and Linkwitz-Riley cascades, a
// crossover, one-pole low-pass, high-pass and all-pass, and a DC blocker.
// The state-variable filter (Svf) is in core/Dsp.h.
//
// Like Svf, each splits coefficients from state: design once, keep one State
// per channel. Designing allocates nothing, so a processor may redesign on
// the audio thread when a parameter moves; for a cutoff that sweeps every
// sample, prefer Svf or OnePole, whose structures stay well-behaved under
// fast modulation where a direct-form biquad's need not.
//
// Biquads and cascades answer response(hz, sampleRate); OnePole has a
// response method per output, and Crossover exposes its low/high cascades.
// These are the exact coefficient responses for drawing curves.
//
//   dsp::Biquad eq = dsp::Biquad::peak(1000.0, 0.7, 6.0, sampleRate);
//   dsp::Biquad::State left, right;
//   y = eq.tick(left, x);
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>

#include "dsp/Math.h"

namespace supple::dsp {

namespace detail {
// Cutoffs are kept inside (0, Nyquist): a design at or past either edge is
// singular.
inline double clampHz(double hz, double sampleRate) { return std::clamp(hz, 1e-3, 0.4999 * sampleRate); }
}  // namespace detail

// A second-order section, transposed direct form II. A first-order filter is
// one with b2 = a2 = 0.
class Biquad {
  public:
    struct State {
        double s1 = 0.0, s2 = 0.0;
    };

    // a0 is normalised to 1.
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    double tick(State& s, double x) const {
        const double y = b0 * x + s.s1;
        s.s1 = b1 * x - a1 * y + s.s2;
        s.s2 = b2 * x - a2 * y;
        return y;
    }
    void process(State& s, double* x, int frames) const {
        for (int i = 0; i < frames; ++i) x[i] = tick(s, x[i]);
    }

    std::complex<double> response(double hz, double sampleRate) const {
        const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / sampleRate), z2 = z1 * z1;
        return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }

    // --- designs: Robert Bristow-Johnson's Audio EQ Cookbook ------------------
    // q is the resonance; for the shelves it sets the slope (0.707 is the
    // steepest without overshoot). gainDb is the boost or cut.

    static Biquad lowpass(double hz, double q, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        return w.normalised((1.0 - w.cos) * 0.5, 1.0 - w.cos, (1.0 - w.cos) * 0.5, 1.0 + w.alpha, -2.0 * w.cos,
                            1.0 - w.alpha);
    }
    static Biquad highpass(double hz, double q, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        return w.normalised((1.0 + w.cos) * 0.5, -(1.0 + w.cos), (1.0 + w.cos) * 0.5, 1.0 + w.alpha, -2.0 * w.cos,
                            1.0 - w.alpha);
    }
    // 0 dB at the centre.
    static Biquad bandpass(double hz, double q, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        return w.normalised(w.alpha, 0.0, -w.alpha, 1.0 + w.alpha, -2.0 * w.cos, 1.0 - w.alpha);
    }
    static Biquad notch(double hz, double q, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        return w.normalised(1.0, -2.0 * w.cos, 1.0, 1.0 + w.alpha, -2.0 * w.cos, 1.0 - w.alpha);
    }
    static Biquad allpass(double hz, double q, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        return w.normalised(1.0 - w.alpha, -2.0 * w.cos, 1.0 + w.alpha, 1.0 + w.alpha, -2.0 * w.cos, 1.0 - w.alpha);
    }
    static Biquad peak(double hz, double q, double gainDb, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        const double a = std::pow(10.0, gainDb / 40.0);
        return w.normalised(1.0 + w.alpha * a, -2.0 * w.cos, 1.0 - w.alpha * a, 1.0 + w.alpha / a, -2.0 * w.cos,
                            1.0 - w.alpha / a);
    }
    static Biquad lowShelf(double hz, double q, double gainDb, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        const double a = std::pow(10.0, gainDb / 40.0), r = 2.0 * std::sqrt(a) * w.alpha;
        return w.normalised(a * ((a + 1.0) - (a - 1.0) * w.cos + r), 2.0 * a * ((a - 1.0) - (a + 1.0) * w.cos),
                            a * ((a + 1.0) - (a - 1.0) * w.cos - r), (a + 1.0) + (a - 1.0) * w.cos + r,
                            -2.0 * ((a - 1.0) + (a + 1.0) * w.cos), (a + 1.0) + (a - 1.0) * w.cos - r);
    }
    static Biquad highShelf(double hz, double q, double gainDb, double sampleRate) {
        const Prewarp w(hz, q, sampleRate);
        const double a = std::pow(10.0, gainDb / 40.0), r = 2.0 * std::sqrt(a) * w.alpha;
        return w.normalised(a * ((a + 1.0) + (a - 1.0) * w.cos + r), -2.0 * a * ((a - 1.0) + (a + 1.0) * w.cos),
                            a * ((a + 1.0) + (a - 1.0) * w.cos - r), (a + 1.0) - (a - 1.0) * w.cos + r,
                            2.0 * ((a - 1.0) - (a + 1.0) * w.cos), (a + 1.0) - (a - 1.0) * w.cos - r);
    }

    // First order, by the bilinear transform, prewarped to hz.
    static Biquad lowpass1(double hz, double sampleRate) {
        const double k = std::tan(kPi * detail::clampHz(hz, sampleRate) / sampleRate);
        Biquad f;
        f.b0 = f.b1 = k / (1.0 + k);
        f.a1 = (k - 1.0) / (k + 1.0);
        return f;
    }
    static Biquad highpass1(double hz, double sampleRate) {
        const double k = std::tan(kPi * detail::clampHz(hz, sampleRate) / sampleRate);
        Biquad f;
        f.b0 = 1.0 / (1.0 + k);
        f.b1 = -f.b0;
        f.a1 = (k - 1.0) / (k + 1.0);
        return f;
    }

  private:
    struct Prewarp {
        double cos, alpha;
        Prewarp(double hz, double q, double sampleRate) {
            const double w0 = 2.0 * kPi * detail::clampHz(hz, sampleRate) / sampleRate;
            cos = std::cos(w0);
            alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
        }
        static Biquad normalised(double b0, double b1, double b2, double a0, double a1, double a2) {
            Biquad f;
            f.b0 = b0 / a0;
            f.b1 = b1 / a0;
            f.b2 = b2 / a0;
            f.a1 = a1 / a0;
            f.a2 = a2 / a0;
            return f;
        }
    };
};

// Up to eight sections in series: a Butterworth or Linkwitz-Riley filter of
// order 1 to 16.
class Cascade {
  public:
    static constexpr int kMaxSections = 8;
    struct State {
        Biquad::State s[kMaxSections];
    };

    Biquad section[kMaxSections];
    int sections = 0;

    double tick(State& s, double x) const {
        for (int k = 0; k < sections; ++k) x = section[k].tick(s.s[k], x);
        return x;
    }
    void process(State& s, double* x, int frames) const {
        for (int k = 0; k < sections; ++k) section[k].process(s.s[k], x, frames);
    }
    std::complex<double> response(double hz, double sampleRate) const {
        std::complex<double> h = 1.0;
        for (int k = 0; k < sections; ++k) h *= section[k].response(hz, sampleRate);
        return h;
    }
    void add(const Biquad& b) {
        if (sections < kMaxSections) section[sections++] = b;
    }
};

// Maximally flat; -3 dB at hz. order 1..16.
struct Butterworth {
    static Cascade lowpass(int order, double hz, double sampleRate) { return design(order, hz, sampleRate, false); }
    static Cascade highpass(int order, double hz, double sampleRate) { return design(order, hz, sampleRate, true); }

    // The Q of each second-order section, lowest first; a first-order section
    // (odd orders) comes first and has none.
    static double sectionQ(int order, int k) {
        const double angle = order % 2 == 0 ? kPi * (2 * k + 1) / (2.0 * order) : kPi * (k + 1) / order;
        return 1.0 / (2.0 * std::cos(angle));
    }

  private:
    static Cascade design(int order, double hz, double sampleRate, bool high) {
        order = std::clamp(order, 1, 2 * Cascade::kMaxSections);
        Cascade c;
        if (order % 2 == 1) c.add(high ? Biquad::highpass1(hz, sampleRate) : Biquad::lowpass1(hz, sampleRate));
        for (int k = 0; k < order / 2; ++k) {
            const double q = sectionQ(order, k);
            c.add(high ? Biquad::highpass(hz, q, sampleRate) : Biquad::lowpass(hz, q, sampleRate));
        }
        return c;
    }
};

// A Butterworth of half the order, twice: -6 dB at hz. For orders 4, 8, 12
// and 16 the low and high halves sum to an all-pass, flat in level.
// Even orders 2..16; orders 2, 6, 10 and 14 need the high half inverted.
struct LinkwitzRiley {
    static Cascade lowpass(int order, double hz, double sampleRate) { return squared(Butterworth::lowpass(half(order), hz, sampleRate)); }
    static Cascade highpass(int order, double hz, double sampleRate) { return squared(Butterworth::highpass(half(order), hz, sampleRate)); }

  private:
    static int half(int order) { return std::clamp(order / 2, 1, Cascade::kMaxSections); }
    static Cascade squared(const Cascade& c) {
        Cascade out;
        for (int k = 0; k < c.sections; ++k) {
            out.add(c.section[k]);
            out.add(c.section[k]);
        }
        return out;
    }
};

// A two-band split (Linkwitz-Riley, order 4 or 8): low + high is the input
// through an all-pass, so splitting, processing each band and summing
// changes nothing when the processing does nothing.
class Crossover {
  public:
    struct State {
        Cascade::State low, high;
    };
    struct Bands {
        double low, high;
    };

    void setCoefficients(double hz, double sampleRate, int order = 4) {
        order = order >= 8 ? 8 : 4;
        low_ = LinkwitzRiley::lowpass(order, hz, sampleRate);
        high_ = LinkwitzRiley::highpass(order, hz, sampleRate);
    }
    Bands tick(State& s, double x) const { return {low_.tick(s.low, x), high_.tick(s.high, x)}; }
    const Cascade& low() const { return low_; }
    const Cascade& high() const { return high_; }

  private:
    Cascade low_, high_;
};

// One pole, topology-preserving (Zavalishin): smooth under modulation, so the
// cutoff may change every sample. Low-pass, high-pass and all-pass outputs.
class OnePole {
  public:
    struct State {
        double s = 0.0;
    };
    struct Outputs {
        double low, high, all;
    };

    void setCutoff(double hz, double sampleRate) {
        g_ = std::tan(kPi * detail::clampHz(hz, sampleRate) / sampleRate);
        gain_ = g_ / (1.0 + g_);
    }
    Outputs tick(State& st, double x) const {
        const double v = (x - st.s) * gain_;
        const double low = v + st.s;
        st.s = low + v;
        return {low, x - low, 2.0 * low - x};
    }
    double lowpass(State& st, double x) const { return tick(st, x).low; }
    double highpass(State& st, double x) const { return tick(st, x).high; }
    double allpass(State& st, double x) const { return tick(st, x).all; }

    std::complex<double> lowpassResponse(double hz, double sampleRate) const { return 1.0 / (1.0 + jw(hz, sampleRate)); }
    std::complex<double> highpassResponse(double hz, double sampleRate) const {
        const std::complex<double> s = jw(hz, sampleRate);
        return s / (1.0 + s);
    }
    std::complex<double> allpassResponse(double hz, double sampleRate) const {
        const std::complex<double> s = jw(hz, sampleRate);
        return (1.0 - s) / (1.0 + s);
    }

  private:
    // The analog prototype's s at this frequency, prewarped (exact; see Svf).
    std::complex<double> jw(double hz, double sampleRate) const {
        return {0.0, std::tan(kPi * std::clamp(hz, 0.0, 0.4999 * sampleRate) / sampleRate) / g_};
    }
    double g_ = 1.0, gain_ = 0.5;
};

// Removes DC and the lowest rumble: y = x - x[-1] + r y[-1], about -3 dB at
// hz (5 to 20 Hz is usual). Needed after anything asymmetric: a folder, a
// rectifier, a biased tube stage.
class DcBlocker {
  public:
    struct State {
        double x1 = 0.0, y1 = 0.0;
    };
    void setCutoff(double hz, double sampleRate) { r_ = std::exp(-2.0 * kPi * detail::clampHz(hz, sampleRate) / sampleRate); }
    double tick(State& s, double x) const {
        const double y = x - s.x1 + r_ * s.y1;
        s.x1 = x;
        s.y1 = y;
        return y;
    }
    std::complex<double> response(double hz, double sampleRate) const {
        const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / sampleRate);
        return (1.0 - z1) / (1.0 - r_ * z1);
    }

  private:
    double r_ = 0.999;
};

}  // namespace supple::dsp
