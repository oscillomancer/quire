// Oscillators (synth.md §4.2). Per-sample, phase in cycles, `dt` = cycles per
// internal sample, `pm` = phase offset in cycles (FM input). RT-safe.
#pragma once

#include <array>
#include <cmath>

#include "dsp.h"
#include "orrery/params.h"
#include "supple/base/rng.h"

namespace supple::orrery {

// Band-limited classic waves (PolyBLEP/BLAMP).
namespace wave {

inline double saw(double t, double dt) SUPPLE_RT { return 2.0 * t - 1.0 - dsp::polyBlep(t, dt); }

inline double pulse(double t, double dt, double w) SUPPLE_RT {
    const double naive = t < w ? 1.0 : -1.0;
    return naive + dsp::polyBlep(t, dt) - dsp::polyBlep(dsp::wrap01(t - w), dt);
}

inline double triangle(double t, double dt) SUPPLE_RT {
    const double naive = t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
    // Slope changes of +8 (at 0) and -8 (at 1/2) per cycle.
    return naive + 8.0 * dt * (dsp::polyBlamp(t, dt) - dsp::polyBlamp(dsp::wrap01(t - 0.5), dt));
}

inline double sine(double t) SUPPLE_RT { return dsp::sin2pi(t); }

inline double classic(Wave w, double t, double dt, double width) SUPPLE_RT {
    switch (w) {
        case Wave::Saw:
            return saw(t, dt);
        case Wave::Square:
            return pulse(t, dt, 0.5);
        case Wave::Pulse:
            return pulse(t, dt, width);
        case Wave::Triangle:
            return triangle(t, dt);
        default:
            return sine(t);
    }
}

}  // namespace wave

// Virtual analog (va) and sub oscillators share the phase accumulator.
class VaOsc {
  public:
    void reset(double phase) SUPPLE_RT { phase_ = dsp::wrap01(phase); }
    double phase() const { return phase_; }

    // shape morphs toward the next wave in the list saw → square → pulse →
    // triangle → sine.
    double next(double dt, Wave w, double width, double shape, double pm) SUPPLE_RT {
        const double t = pm == 0.0 ? phase_ : dsp::wrap01(phase_ + pm);
        double v = wave::classic(w, t, dt, width);
        if (shape > 0.0 && w < Wave::Sine) {
            const auto nextWave = static_cast<Wave>(static_cast<int>(w) + 1);
            v += shape * (wave::classic(nextWave, t, dt, width) - v);
        }
        advance(dt);
        return v;
    }

    // Returns the fraction of the sample after the wrap (0 < f <= 1) if the
    // phase wrapped during the last advance, else 0 — the hard-sync signal.
    double wrapped() const { return wrapFrac_; }
    // Hard sync: restart at the master's wrap point (frac of a sample ago).
    void syncTo(double frac, double dt) SUPPLE_RT { phase_ = dsp::wrap01(frac * dt); }

  private:
    void advance(double dt) SUPPLE_RT {
        phase_ += dt;
        wrapFrac_ = 0.0;
        if (phase_ >= 1.0) {
            phase_ -= std::floor(phase_);
            wrapFrac_ = dt > 0.0 ? phase_ / dt : 0.0;
        }
    }
    double phase_ = 0.0, wrapFrac_ = 0.0;
};

// FM/PM operator: sine variants with self-feedback.
class FmOsc {
  public:
    void reset(double phase) SUPPLE_RT {
        phase_ = dsp::wrap01(phase);
        y1_ = y2_ = 0.0;
    }
    double next(double dt, Wave w, double feedback, double pm) SUPPLE_RT {
        const double fb = feedback * dsp::kPi * 0.5 * (y1_ + y2_);
        const double s = dsp::sin2pi(phase_ + pm + fb / dsp::kTwoPi);
        double v = s;
        switch (w) {
            case Wave::HalfSine:
                v = s > 0.0 ? s : 0.0;
                break;
            case Wave::AbsSine:
                v = std::fabs(s);
                break;
            case Wave::SquareSine:
                v = s * std::fabs(s);
                break;
            default:
                break;
        }
        y2_ = y1_;
        y1_ = s;
        phase_ = dsp::wrap01(phase_ + dt);
        return v;
    }

  private:
    double phase_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

// Seeded noise: white, pink (Voss-McCartney, 16 rows), brown (leaky
// integrator), blue (differentiated pink).
class NoiseOsc {
  public:
    void seed(std::uint64_t s) SUPPLE_RT {
        rng_.reseed(s);
        rows_.fill(0.0);
        sum_ = 0.0;
        counter_ = 0;
        brown_ = 0.0;
        lastPink_ = 0.0;
    }
    double next(NoiseColor c) SUPPLE_RT {
        const double white = rng_.uniform(-1.0, 1.0);
        switch (c) {
            case NoiseColor::White:
                return white;
            case NoiseColor::Pink:
                return pink(white);
            case NoiseColor::Brown:
                brown_ = std::clamp(0.996 * brown_ + 0.06 * white, -1.0, 1.0);
                return brown_ * 2.5;
            case NoiseColor::Blue: {
                const double p = pink(white);
                const double v = p - lastPink_;
                lastPink_ = p;
                return v * 0.7;
            }
        }
        return white;
    }

  private:
    double pink(double white) SUPPLE_RT {
        ++counter_;
        const int row = std::countr_zero(counter_ | (1u << 15));
        const double r = rng_.uniform(-1.0, 1.0);
        sum_ += r - rows_[static_cast<std::size_t>(row)];
        rows_[static_cast<std::size_t>(row)] = r;
        return (sum_ + white) * 0.25;
    }

    Rng rng_;
    std::array<double, 16> rows_{};
    double sum_ = 0.0, brown_ = 0.0, lastPink_ = 0.0;
    std::uint32_t counter_ = 0;
};

}  // namespace supple::orrery
