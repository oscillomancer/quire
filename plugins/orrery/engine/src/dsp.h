// Small DSP helpers shared by Orrery's modules. Header-only, RT-safe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "supple/base/rt.h"

namespace supple::orrery::dsp {

inline constexpr double kPi = std::numbers::pi;
inline constexpr double kTwoPi = 2.0 * std::numbers::pi;
// Control ticks: modulation is evaluated every kTick base-rate samples, on a
// grid anchored at each voice's start (voices) or at absolute sample 0
// (global modulators, FX parameters) — never at host block boundaries, so
// renders do not depend on the block size.
inline constexpr std::uint32_t kTick = 16;

inline double dbToGain(double db) SUPPLE_RT { return std::pow(10.0, db / 20.0); }
// Level fields are off at their minimum (-60 dB).
inline double levelGain(double db) SUPPLE_RT { return db <= -60.0 ? 0.0 : dbToGain(db); }
inline double pitchToHz(double semitones) SUPPLE_RT {
    return 440.0 * std::exp2((semitones - 69.0) / 12.0);
}

// Constant-power pan, p in -1..1 → (left, right) gains, both sqrt(1/2) at 0.
struct PanGains {
    double l, r;
};
inline PanGains pan(double p) SUPPLE_RT {
    // Symmetric evaluation so equal positions give bit-equal gains (centre
    // exactly sqrt(1/2) on both sides).
    const double a = std::clamp(p, -1.0, 1.0) * 0.25 * kPi;  // -π/4..π/4
    const double c = std::cos(a) * std::numbers::sqrt2 * 0.5,
                 s = std::sin(a) * std::numbers::sqrt2 * 0.5;
    return {c - s, c + s};
}

// Segment shaping for envelopes: x in 0..1, c in -1..1. c = 0 is linear;
// c > 0 bends toward exponential growth (slow start, fast end).
inline double shapeRise(double x, double c) SUPPLE_RT {
    if (std::fabs(c) < 1e-6) return x;
    const double k = c * 6.0;
    return std::expm1(k * x) / std::expm1(k);
}
// For falling segments "exponential" means fast start, slow tail.
inline double shapeFall(double x, double c) SUPPLE_RT { return 1.0 - shapeRise(1.0 - x, c); }

// Response curve on a modulation source value (0..1 or -1..1, sign kept).
inline double sourceCurve(double v, double curve) SUPPLE_RT {
    if (curve == 0.0) return v;
    const double a = std::fabs(v);
    const double k = curve / 100.0 * 4.0;
    const double y = std::expm1(k * a) / std::expm1(k);
    return v < 0.0 ? -y : y;
}

// PolyBLEP residual for a unit step at phase 0; t = phase in [0,1), dt = phase
// increment per sample.
inline double polyBlep(double t, double dt) SUPPLE_RT {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}
// PolyBLAMP residual (integrated BLEP) for a unit slope change at phase 0.
inline double polyBlamp(double t, double dt) SUPPLE_RT {
    if (t < dt) {
        t = t / dt - 1.0;
        return -1.0 / 3.0 * t * t * t;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt + 1.0;
        return 1.0 / 3.0 * t * t * t;
    }
    return 0.0;
}

inline double wrap01(double x) SUPPLE_RT { return x - std::floor(x); }

// tanh via a [7/6] Padé approximant, exact ±1 beyond |x| = 4.97 (max error
// ~2e-4 near the clamp, far smaller for |x| < 3). Monotonic and smooth.
inline double fastTanh(double x) SUPPLE_RT {
    if (x > 4.97) return 1.0;
    if (x < -4.97) return -1.0;
    const double x2 = x * x;
    return x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2))) /
           (135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2)));
}

// sin(2π·t) for any t (cycles): reduction to a quarter wave and the Taylor
// series through t^11 (error < 4e-8, below -140 dB).
inline double sin2pi(double t) SUPPLE_RT {
    double x = t - std::floor(t);  // 0..1
    if (x > 0.5) x -= 1.0;         // -0.5..0.5
    if (x > 0.25)
        x = 0.5 - x;  // fold to -0.25..0.25
    else if (x < -0.25)
        x = -0.5 - x;
    const double w = kTwoPi * x, w2 = w * w;
    return w * (1.0 + w2 * (-1.0 / 6 +
                            w2 * (1.0 / 120 + w2 * (-1.0 / 5040 + w2 * (1.0 / 362880 +
                                                                        w2 * (-1.0 / 39916800))))));
}

// Linear per-sample ramp across one control tick.
struct Lerp {
    double value = 0.0, step = 0.0;
    void set(double from, double to, double samples) SUPPLE_RT {
        value = from;
        step = (to - from) / samples;
    }
    void snap(double v) SUPPLE_RT {
        value = v;
        step = 0.0;
    }
    double next() SUPPLE_RT {
        const double v = value;
        value += step;
        return v;
    }
};

// Safety soft clipper: identity below -1 dBFS, then a tanh knee that reaches
// full scale asymptotically (slope-continuous at the threshold).
inline float safetyClip(float x) SUPPLE_RT {
    constexpr float t = 0.891250938f;  // -1 dBFS
    const float a = std::fabs(x);
    if (a <= t) return x;
    const float y = t + (1.0f - t) * std::tanh((a - t) / (1.0f - t));
    return x < 0.0f ? -y : y;
}

// SplitMix64 step for deriving per-note seeds.
inline std::uint64_t mixSeed(std::uint64_t a, std::uint64_t b) SUPPLE_RT {
    std::uint64_t z = a + 0x9E3779B97F4A7C15ull * (b + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace supple::orrery::dsp
