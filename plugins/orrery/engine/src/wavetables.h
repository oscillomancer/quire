// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Orrery's wavetables (synth.md §4.2 `wavetable`): original tables generated
// from harmonic recipes — no third-party data — stored as mip-mapped,
// band-limited single-cycle frames.
//
// Each table has kFrames frames (the `position` parameter sweeps them) and
// kLevels mip levels: level L holds at most kTopHarmonics >> L harmonics of the
// table's fundamental, sampled at ≥ 8 samples per top harmonic. The oscillator
// reads the highest level whose top harmonic stays below the internal Nyquist,
// so it never aliases. Tables with inharmonic content (bell, the organ's 16'
// drawbar) set `multiplier`: the table's fundamental is the played pitch
// divided by it, so partial ratios quantize to 1/multiplier steps.
//
// Generation (FFT, ~20 ms per table) runs once per process on the first
// program that uses a wavetable oscillator — on the main thread; the audio
// thread only reads.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp.h"
#include "orrery/params.h"

namespace supple::orrery {

struct Wavetable {
    static constexpr int kFrames = 32;
    static constexpr int kLevels = 10;
    static constexpr int kTopHarmonics = 512;  // level 0

    int multiplier = 1;
    std::array<std::uint32_t, kLevels> size{};  // samples per frame at each level
    // Level L: kFrames frames of size[L] + 1 samples (the last repeats the first).
    std::array<std::vector<float>, kLevels> data;

    const float* frame(int level, int f) const {
        return data[static_cast<std::size_t>(level)].data() +
               static_cast<std::size_t>(f) * (size[static_cast<std::size_t>(level)] + 1);
    }
};

inline constexpr int kTableCount = 7;  // Table enum

class WavetableBank {
  public:
    // All tables, generated on first call (thread-safe; call from the main thread).
    static const WavetableBank& get();
    const Wavetable& table(Table t) const { return tables_[static_cast<std::size_t>(t)]; }

  private:
    WavetableBank();
    std::array<Wavetable, kTableCount> tables_;
};

// The harmonic spectrum of one frame: amplitude and phase (radians, sine
// convention: x(t) = Σ a_k sin(2πkt + φ_k)) for k = 1..kTopHarmonics.
struct Spectrum {
    std::vector<double> amplitude, phase;
};
// The recipe of each table at position p ∈ [0, 1] (exposed for tests).
Spectrum tableSpectrum(Table t, double p);
int tableMultiplier(Table t);

// The highest harmonic frequency to keep, in cycles per internal sample: the
// audible band (20 kHz) or the internal Nyquist, whichever is lower. Keeping
// harmonics out of the decimator's transition band (base Nyquist .. ~28 kHz)
// is what keeps oversampled wavetables alias-free.
inline double wavetableLimit(double internalRate) { return std::min(0.5, 20000.0 / internalRate); }

// Wavetable oscillator: phase in table cycles.
class WtOsc {
  public:
    void reset(double phase) SUPPLE_RT { phase_ = dsp::wrap01(phase); }
    double phase() const { return phase_; }

    // dt: cycles of the played pitch per internal sample; position 0..1; pm:
    // phase offset in cycles of the played pitch; limit: wavetableLimit().
    double next(double dt, const Wavetable& t, double position, double pm, double limit) SUPPLE_RT {
        const double m = static_cast<double>(t.multiplier);
        const double tdt = dt / m;
        double tp = phase_;
        if (pm != 0.0) tp = dsp::wrap01(tp + pm / m);
        phase_ += tdt;
        if (phase_ >= 1.0) phase_ -= 1.0;
        // Highest level whose top harmonic (512 >> L) × tdt stays ≤ limit.
        int e = 0;
        const double x = static_cast<double>(Wavetable::kTopHarmonics) * tdt / limit;
        int level = 0;
        if (x > 1.0) {
            const double mant = std::frexp(x, &e);  // x = mant·2^e, mant ∈ [0.5, 1)
            level = mant > 0.5 ? e : e - 1;
        }
        if (level >= Wavetable::kLevels) level = Wavetable::kLevels - 1;
        const double fpos = std::clamp(position, 0.0, 1.0) * (Wavetable::kFrames - 1);
        const int f0 = static_cast<int>(fpos);
        const double fr = fpos - f0;
        const std::uint32_t n = t.size[static_cast<std::size_t>(level)];
        const double idx = tp * n;
        const auto i = static_cast<std::uint32_t>(idx);
        const double a = idx - i;
        auto at = [&](const float* w, std::uint32_t k) { return static_cast<double>(w[k]); };
        const float* w0 = t.frame(level, f0);
        const double v0 = at(w0, i) + a * (at(w0, i + 1) - at(w0, i));
        if (fr <= 0.0 || f0 >= Wavetable::kFrames - 1) return v0;
        const float* w1 = t.frame(level, f0 + 1);
        const double v1 = at(w1, i) + a * (at(w1, i + 1) - at(w1, i));
        return v0 + fr * (v1 - v0);
    }

  private:
    double phase_ = 0.0;
};

}  // namespace supple::orrery
