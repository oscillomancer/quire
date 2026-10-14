// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Per-voice filters and drive (synth.md §4.3–4.4). Mono objects (a voice
// holds one per channel). Coefficients are updated per control tick; the
// per-sample loop takes already-ramped values. RT-safe.
#pragma once

#include <array>
#include <cmath>
#include <span>

#include "dsp.h"
#include "orrery/params.h"

namespace supple::orrery {

// Prewarped TPT integrator gain for cutoff `hz` at rate `fs`.
inline double tptG(double hz, double fs) SUPPLE_RT {
    return std::tan(dsp::kPi * std::clamp(hz, 5.0, 0.49 * fs) / fs);
}

// Zero-delay-feedback state-variable filter (trapezoidal integration).
class Svf {
  public:
    void reset() SUPPLE_RT { ic1_ = ic2_ = 0.0; }
    // g = tptG(), k = 2 - 2 * resonance (resonance 0..~1).
    double next(double x, double g, double k, SvfMode mode) SUPPLE_RT {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2_;
        const double v1 = a1 * ic1_ + a2 * v3;
        const double v2 = ic2_ + a2 * ic1_ + a3 * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        const double lp = v2, bp = v1, hp = x - k * v1 - v2;
        switch (mode) {
            case SvfMode::Lowpass:
                return lp;
            case SvfMode::Bandpass:
                return bp;
            case SvfMode::Highpass:
                return hp;
            case SvfMode::Notch:
                return lp + hp;
            case SvfMode::Peak:
                return lp - hp;
            case SvfMode::Allpass:
                return lp + hp - k * bp;
        }
        return lp;
    }
    // Constant-peak-gain band-pass for formant banks: k = 1/Q.
    double bandpass(double x, double g, double k) SUPPLE_RT {
        return next(x, g, k, SvfMode::Bandpass) * k;
    }

  private:
    double ic1_ = 0.0, ic2_ = 0.0;
};

// Transistor-ladder low-pass: four TPT one-poles with global feedback solved
// in closed form (zero delay), input saturation. 12 dB taps stage 2.
class Ladder {
  public:
    void reset() SUPPLE_RT { s_.fill(0.0f); }
    // k = 0..4 (self-oscillation near 4); drive = input gain (linear).
    double next(double input, double coef, double feedback, double gain, bool db24) SUPPLE_RT {
        const float x = static_cast<float>(input), g = static_cast<float>(coef),
                    k = static_cast<float>(feedback), drive = static_cast<float>(gain);
        const float G = g / (1.0f + g), B = 1.0f / (1.0f + g);
        const float S = G * G * G * B * s_[0] + G * G * B * s_[1] + G * B * s_[2] + B * s_[3];
        const float G4 = G * G * G * G;
        const float y4 = (G4 * x * drive + S) / (1.0f + k * G4);
        float u = saturate(x * drive - k * y4);
        float out2 = 0.0f;
        for (std::size_t i = 0; i < 4; ++i) {
            const float v = (u - s_[i]) * G;
            const float y = v + s_[i];
            s_[i] = y + v;
            u = y;
            if (i == 1) out2 = y;
        }
        return static_cast<double>((db24 ? u : out2) * (1.0f + 0.5f * k) /
                                   std::max(1.0f, drive * 0.5f + 0.5f));
    }

    struct Lane {
        Ladder* filter;
        const double* input;
        double* output;
        const double *g, *k, *drive, *level;
    };

    // Phase and coefficient ramps remain double precision. Filter recursions
    // use float in both paths, so a group/segment boundary never rounds state
    // differently. Transpose only at segment boundaries; independent lanes
    // let the compiler emit baseline SIMD with a portable scalar fallback.
    template <std::size_t N>
    static void runGroup(const Lane* lanes, std::size_t count, std::uint32_t frames,
                         bool db24) SUPPLE_RT {
        float state[4][N]{};
        for (std::size_t j = 0; j < count; ++j)
            for (std::size_t i = 0; i < 4; ++i) state[i][j] = lanes[j].filter->s_[i];
        for (std::uint32_t s = 0; s < frames; ++s) {
            float x[N], g[N], k[N], d[N], out[N];
            for (std::size_t j = 0; j < N; ++j) {
                const Lane& l = lanes[j < count ? j : 0];
                x[j] = static_cast<float>(l.input[s]);
                g[j] = static_cast<float>(l.g[s]);
                k[j] = static_cast<float>(l.k[s]);
                d[j] = static_cast<float>(l.drive[s]);
            }
#if defined(__clang__) && !defined(SUPPLE_RTSAN_ENABLED)
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
#endif
            for (std::size_t j = 0; j < N; ++j) {
                const float G = g[j] / (1.0f + g[j]), B = 1.0f / (1.0f + g[j]);
                const float S = G * G * G * B * state[0][j] + G * G * B * state[1][j] +
                                G * B * state[2][j] + B * state[3][j];
                const float G4 = G * G * G * G;
                const float y4 = (G4 * x[j] * d[j] + S) / (1.0f + k[j] * G4);
                float u = saturate(x[j] * d[j] - k[j] * y4);
                float out2 = 0.0f;
                for (std::size_t i = 0; i < 4; ++i) {
                    const float v = (u - state[i][j]) * G;
                    const float y = v + state[i][j];
                    state[i][j] = y + v;
                    u = y;
                    if (i == 1) out2 = y;
                }
                out[j] =
                    (db24 ? u : out2) * (1.0f + 0.5f * k[j]) / std::max(1.0f, d[j] * 0.5f + 0.5f);
            }
            for (std::size_t j = 0; j < count; ++j)
                lanes[j].output[s] = static_cast<double>(out[j]) * lanes[j].level[s];
        }
        for (std::size_t j = 0; j < count; ++j)
            for (std::size_t i = 0; i < 4; ++i) lanes[j].filter->s_[i] = state[i][j];
    }

  private:
    static float saturate(float x) SUPPLE_RT {
        if (x > 4.97f) return 1.0f;
        if (x < -4.97f) return -1.0f;
        const float x2 = x * x;
        return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) /
               (135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2)));
    }
    std::array<float, 4> s_{};
};

// Feedback comb with damping in the loop and a fractional delay.
class Comb {
  public:
    void attach(std::span<float> buffer) SUPPLE_RT {
        buf_ = buffer;
        reset();
    }
    void reset() SUPPLE_RT {
        for (float& f : buf_) f = 0.0f;
        pos_ = 0;
        lp_ = 0.0;
    }
    // Copies another comb's state (not its buffer binding).
    void copyState(const Comb& o) SUPPLE_RT {
        std::copy_n(o.buf_.begin(), std::min(buf_.size(), o.buf_.size()), buf_.begin());
        pos_ = o.pos_;
        lp_ = o.lp_;
    }
    // delay in samples (fractional), feedback -0.99..0.99, damping 0..1.
    double next(double x, double delay, double feedback, double damping) SUPPLE_RT {
        if (buf_.size() < 4) return x;
        const double d = std::clamp(delay, 1.0, static_cast<double>(buf_.size() - 2));
        const double rp = static_cast<double>(pos_) - d + static_cast<double>(buf_.size());
        const auto i0 = static_cast<std::size_t>(rp) % buf_.size();
        const std::size_t i1 = (i0 + 1) % buf_.size();
        const double frac = rp - std::floor(rp);
        const auto a = static_cast<double>(buf_[i0]), b = static_cast<double>(buf_[i1]);
        const double tap = a + (b - a) * frac;
        lp_ += (tap - lp_) * (1.0 - damping * 0.95);
        const double y = x + feedback * lp_;
        buf_[pos_] = static_cast<float>(y);
        pos_ = (pos_ + 1) % buf_.size();
        return y * (1.0 - std::fabs(feedback) * 0.5);
    }

  private:
    std::span<float> buf_;
    std::size_t pos_ = 0;
    double lp_ = 0.0;
};

// Vowel formant bank: three constant-peak band-passes (Peterson & Barney
// 1952 adult male averages, vowels ordered a e i o u).
class Formant {
  public:
    void reset() SUPPLE_RT {
        for (auto& f : bands_) f.reset();
    }
    struct Coefs {
        std::array<double, 3> g, k, gain;
    };
    static Coefs coefs(double vowel, double shiftSt, double fs) SUPPLE_RT {
        static constexpr double f[5][3] = {{730, 1090, 2440},
                                           {530, 1840, 2480},
                                           {270, 2290, 3010},
                                           {570, 840, 2410},
                                           {300, 870, 2240}};
        static constexpr double bw[3] = {80, 90, 120};
        static constexpr double gain[3] = {1.0, 0.5, 0.25};
        const double v = std::clamp(vowel, 0.0, 4.0);
        const int i = std::min(3, static_cast<int>(v));
        const double t = v - i;
        const double shift = std::exp2(shiftSt / 12.0);
        Coefs c;
        for (std::size_t b = 0; b < 3; ++b) {
            const double hz = (f[i][b] + (f[i + 1][b] - f[i][b]) * t) * shift;
            c.g[b] = tptG(hz, fs);
            c.k[b] = bw[b] / hz;
            c.gain[b] = gain[b];
        }
        return c;
    }
    double next(double x, const Coefs& c) SUPPLE_RT {
        double y = 0.0;
        for (std::size_t b = 0; b < 3; ++b) y += bands_[b].bandpass(x, c.g[b], c.k[b]) * c.gain[b];
        return y * 1.5;
    }

  private:
    std::array<Svf, 3> bands_{};
};

// Waveshaping drive (synth.md §4.4); gain-compensated, bias for asymmetry.
enum class DriveCurve : std::uint8_t { Soft, Tape, Fold, Hard };
// `comp` = the output gain compensation, 1/sqrt(max(1, gain)).
[[gnu::always_inline]] inline double drive(double x, DriveCurve curve, double gain, double bias,
                                           double comp) SUPPLE_RT {
    const double u = x * gain;
    double y = 0.0;
    switch (curve) {
        case DriveCurve::Soft:
            y = dsp::fastTanh(u + bias) - dsp::fastTanh(bias);
            break;
        case DriveCurve::Tape: {
            // Asymmetric soft knee: the positive side saturates earlier.
            const double v = u + bias;
            y = (v >= 0.0 ? dsp::fastTanh(v * 1.2) / 1.2 : dsp::fastTanh(v * 0.8) / 0.8) -
                (bias >= 0.0 ? dsp::fastTanh(bias * 1.2) / 1.2 : dsp::fastTanh(bias * 0.8) / 0.8);
            break;
        }
        case DriveCurve::Fold:
            y = dsp::sin2pi(0.25 * (u + bias)) - dsp::sin2pi(0.25 * bias);
            break;
        case DriveCurve::Hard:
            y = std::clamp(u + bias, -1.0, 1.0) - std::clamp(bias, -1.0, 1.0);
            break;
    }
    return y * comp;
}

template <DriveCurve C, bool ZeroBias>
inline void driveBlock(double* y, std::uint32_t n, const double* gain, const double* bias,
                       const double* comp, const double* mix) SUPPLE_RT {
    if constexpr (C == DriveCurve::Fold) {
        // Baseline SSE2 lacks vector floor for the fold's phase reduction.
        for (std::uint32_t s = 0; s < n; ++s)
            y[s] += (drive(y[s], C, gain[s], ZeroBias ? 0.0 : bias[s], comp[s]) - y[s]) * mix[s];
    } else {
#if defined(__clang__) && !defined(SUPPLE_RTSAN_ENABLED)
#pragma clang loop vectorize(enable) vectorize_width(2) interleave_count(1)
#endif
        for (std::uint32_t s = 0; s < n; ++s)
            y[s] += (drive(y[s], C, gain[s], ZeroBias ? 0.0 : bias[s], comp[s]) - y[s]) * mix[s];
    }
}

template <bool ZeroBias>
inline void driveBlock(DriveCurve curve, double* y, std::uint32_t n, const double* gain,
                       const double* bias, const double* comp, const double* mix) SUPPLE_RT {
    switch (curve) {
        case DriveCurve::Soft:
            driveBlock<DriveCurve::Soft, ZeroBias>(y, n, gain, bias, comp, mix);
            break;
        case DriveCurve::Tape:
            driveBlock<DriveCurve::Tape, ZeroBias>(y, n, gain, bias, comp, mix);
            break;
        case DriveCurve::Fold:
            driveBlock<DriveCurve::Fold, ZeroBias>(y, n, gain, bias, comp, mix);
            break;
        case DriveCurve::Hard:
            driveBlock<DriveCurve::Hard, ZeroBias>(y, n, gain, bias, comp, mix);
            break;
    }
}

}  // namespace supple::orrery
