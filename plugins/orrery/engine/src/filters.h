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
    void reset() SUPPLE_RT { s_.fill(0.0); }
    // k = 0..4 (self-oscillation near 4); drive = input gain (linear).
    double next(double x, double g, double k, double drive, bool db24) SUPPLE_RT {
        const double G = g / (1.0 + g), B = 1.0 / (1.0 + g);
        const double S = G * G * G * B * s_[0] + G * G * B * s_[1] + G * B * s_[2] + B * s_[3];
        const double G4 = G * G * G * G;
        const double y4 = (G4 * x * drive + S) / (1.0 + k * G4);
        double u = dsp::fastTanh(x * drive - k * y4);
        double out2 = 0.0, out4 = 0.0;
        for (std::size_t i = 0; i < 4; ++i) {
            const double v = (u - s_[i]) * G;
            const double y = v + s_[i];
            s_[i] = y + v;
            u = y;
            if (i == 1) out2 = y;
            if (i == 3) out4 = y;
        }
        // Passband gain drops with resonance; compensate half of it.
        const double comp = 1.0 + 0.5 * k;
        return (db24 ? out4 : out2) * comp / std::max(1.0, drive * 0.5 + 0.5);
    }

  private:
    std::array<double, 4> s_{};
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
inline double drive(double x, DriveCurve curve, double gain, double bias, double comp) SUPPLE_RT {
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

}  // namespace supple::orrery
