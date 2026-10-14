// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "wavetables.h"

#include <algorithm>
#include <complex>
#include <numbers>

#include "supple/dsp/dsp.h"

namespace supple::orrery {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr int kTop = Wavetable::kTopHarmonics;

std::uint32_t levelSize(int level) {
    const int harmonics = kTop >> level;
    return static_cast<std::uint32_t>(std::max(256, 8 * harmonics));
}

Spectrum empty() {
    Spectrum s;
    s.amplitude.assign(kTop + 1, 0.0);
    s.phase.assign(kTop + 1, 0.0);
    return s;
}

// harmonic_ramp: saw (1/k) at 0 rolling off to a sine at 1.
Spectrum harmonicRamp(double p) {
    Spectrum s = empty();
    const double exponent = 1.0 + 8.0 * p * p;
    for (int k = 1; k <= kTop; ++k) {
        s.amplitude[static_cast<std::size_t>(k)] = std::pow(static_cast<double>(k), -exponent);
        s.phase[static_cast<std::size_t>(k)] = kPi;  // a rising ramp: -Σ sin(2πkt)/k
    }
    return s;
}

// pwm: a pulse whose duty narrows from 50 % to 5 %.
Spectrum pwm(double p) {
    Spectrum s = empty();
    const double w = 0.5 - 0.45 * p;
    for (int k = 1; k <= kTop; ++k) {
        const double a = 4.0 / (kPi * k) * std::sin(kPi * k * w);
        // A cos(2πk(t - w/2)) = A sin(2πkt - πkw + π/2)
        s.amplitude[static_cast<std::size_t>(k)] = std::fabs(a);
        s.phase[static_cast<std::size_t>(k)] = -kPi * k * w + kPi / 2 + (a < 0.0 ? kPi : 0.0);
    }
    return s;
}

// formant: a bright source shaped by vowel formants a → e → i → o → u for a
// nominal 110 Hz fundamental (average adult formant frequencies, rounded).
Spectrum formant(double p) {
    constexpr double kVowels[5][3] = {{730, 1090, 2440},
                                      {530, 1840, 2480},
                                      {270, 2290, 3010},
                                      {570, 840, 2410},
                                      {300, 870, 2240}};
    constexpr double kBandwidth[3] = {90, 110, 160};
    constexpr double kGain[3] = {1.0, 0.5, 0.3};
    constexpr double kNominal = 110.0;
    const double x = p * 4.0;
    const int v = std::min(3, static_cast<int>(x));
    const double fr = x - v;
    double f[3];
    for (int j = 0; j < 3; ++j) f[j] = kVowels[v][j] + fr * (kVowels[v + 1][j] - kVowels[v][j]);
    Spectrum s = empty();
    for (int k = 1; k <= kTop; ++k) {
        const double hz = k * kNominal;
        double env = 0.03;
        for (int j = 0; j < 3; ++j) {
            const double d = (hz - f[j]) / kBandwidth[j];
            env += kGain[j] / (1.0 + d * d);
        }
        s.amplitude[static_cast<std::size_t>(k)] = env / std::sqrt(static_cast<double>(k));
    }
    return s;
}

// bell: an inharmonic partial set (hum, prime, tierce, quint, nominal, …) with
// ratios quantized to eighths of the played pitch; position brightens and
// slightly stretches the upper partials.
Spectrum bell(double p) {
    constexpr double kPartials[][2] = {{0.5, 0.6}, {1.0, 1.0},   {1.2, 0.8},  {1.5, 0.5},
                                       {2.0, 0.9}, {2.5, 0.35},  {2.67, 0.3}, {3.0, 0.25},
                                       {4.0, 0.2}, {5.33, 0.12}, {6.5, 0.08}, {8.0, 0.05}};
    Spectrum s = empty();
    const double m = tableMultiplier(Table::Bell);
    int i = 0;
    for (const auto& part : kPartials) {
        const double ratio = part[0] * (1.0 + 0.04 * p * (part[0] - 1.0));
        const int k = static_cast<int>(std::lround(ratio * m));
        const double dark = std::exp(-static_cast<double>(i) * (1.0 - p) * 0.35);
        if (k >= 1 && k <= kTop) s.amplitude[static_cast<std::size_t>(k)] += part[1] * dark;
        ++i;
    }
    return s;
}

// organ: nine drawbars (16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1') at harmonics
// 1, 3, 2, 4, 6, 8, 10, 12, 16 of half the played pitch; position crossfades
// through eight registrations, 3 dB per drawbar step.
Spectrum organ(double p) {
    constexpr int kHarmonic[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
    constexpr int kRegistrations[8][9] = {{0, 0, 8, 0, 0, 0, 0, 0, 0}, {0, 0, 8, 8, 0, 0, 0, 0, 0},
                                          {6, 0, 8, 6, 0, 4, 0, 0, 0}, {8, 4, 8, 6, 0, 0, 0, 0, 0},
                                          {8, 8, 8, 0, 0, 0, 0, 0, 0}, {8, 8, 8, 8, 0, 0, 0, 0, 4},
                                          {8, 6, 8, 8, 6, 8, 0, 4, 8}, {8, 8, 8, 8, 8, 8, 8, 8, 8}};
    const double x = p * 7.0;
    const int r = std::min(6, static_cast<int>(x));
    const double fr = x - r;
    Spectrum s = empty();
    for (int d = 0; d < 9; ++d) {
        const double level =
            kRegistrations[r][d] + fr * (kRegistrations[r + 1][d] - kRegistrations[r][d]);
        if (level <= 0.0) continue;
        s.amplitude[static_cast<std::size_t>(kHarmonic[d])] +=
            std::pow(10.0, -(8.0 - level) * 3.0 / 20.0);
    }
    return s;
}

// digital: a ramp through a sample-and-hold and a bit quantizer that coarsen
// with position (5 → 3 bits, 16 → 6 steps per cycle), then band-limited.
// Coarse from the start: at 16 bits and 4096 steps (the first design) the
// low positions were indistinguishable from harmonic_ramp's saw (Nate's
// listening check, 2026-09-30).
Spectrum digital(double p) {
    constexpr std::size_t kN = 4096;
    const double bits = 5.0 - 2.0 * p;
    const double levels = std::exp2(bits - 1.0);
    const auto steps = static_cast<std::size_t>(std::lround(std::exp2(4.0 - 1.415 * p)));
    const std::size_t hold = kN / std::max<std::size_t>(1, steps);
    std::vector<double> x(kN);
    for (std::size_t n = 0; n < kN; ++n) {
        const double t = static_cast<double>((n / hold) * hold) / kN;
        const double v = 2.0 * t - 1.0;
        x[n] = std::round(v * levels) / levels;
    }
    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= kN;
    for (double& v : x) v -= mean;
    supple::dsp::Fft fft;
    fft.prepare(kN);
    std::vector<std::complex<double>> bins(kN / 2 + 1);
    fft.forward(x, bins);
    Spectrum s = empty();
    for (int k = 1; k <= kTop; ++k) {
        // X_k = (N/2)·a·e^{i(φ-π/2)} for a sin(2πkt + φ)
        const std::complex<double> c = bins[static_cast<std::size_t>(k)] * (2.0 / kN);
        s.amplitude[static_cast<std::size_t>(k)] = std::abs(c);
        s.phase[static_cast<std::size_t>(k)] = std::arg(c) + kPi / 2;
    }
    return s;
}

// spectral_comb: a 1/√k series through a raised-cosine comb whose period
// narrows from 16 to 3 harmonics (the fundamental always on a peak).
Spectrum spectralComb(double p) {
    const double period = 16.0 - 13.0 * p;
    Spectrum s = empty();
    for (int k = 1; k <= kTop; ++k) {
        const double c = 0.5 + 0.5 * std::cos(2.0 * kPi * (k - 1) / period);
        s.amplitude[static_cast<std::size_t>(k)] = c * c / std::pow(static_cast<double>(k), 0.6);
    }
    return s;
}

// One level of one frame: the harmonics up to `harmonics`, `n` samples.
std::vector<double> render(const Spectrum& s, int harmonics, std::uint32_t n) {
    std::vector<std::complex<double>> bins(n / 2 + 1);
    const double scale = static_cast<double>(n) / 2.0;
    for (int k = 1; k <= harmonics && static_cast<std::uint32_t>(k) < n / 2; ++k) {
        const auto u = static_cast<std::size_t>(k);
        bins[u] = std::polar(s.amplitude[u] * scale, s.phase[u] - kPi / 2);
    }
    std::vector<double> out(n);
    supple::dsp::Fft fft;
    fft.prepare(n);
    fft.inverse(bins, out);
    return out;
}

Wavetable build(Table t) {
    Wavetable w;
    w.multiplier = tableMultiplier(t);
    for (int l = 0; l < Wavetable::kLevels; ++l) {
        w.size[static_cast<std::size_t>(l)] = levelSize(l);
        w.data[static_cast<std::size_t>(l)].assign(
            static_cast<std::size_t>(Wavetable::kFrames) * (levelSize(l) + 1), 0.0f);
    }
    for (int f = 0; f < Wavetable::kFrames; ++f) {
        const Spectrum s = tableSpectrum(t, static_cast<double>(f) / (Wavetable::kFrames - 1));
        // One gain per frame (from the full-band level) keeps levels consistent.
        const std::vector<double> full = render(s, kTop, levelSize(0));
        double peak = 0.0;
        for (double v : full) peak = std::max(peak, std::fabs(v));
        const double gain = peak > 0.0 ? 1.0 / peak : 0.0;
        for (int l = 0; l < Wavetable::kLevels; ++l) {
            const std::uint32_t n = levelSize(l);
            const std::vector<double> x = l == 0 ? full : render(s, kTop >> l, n);
            float* dst =
                w.data[static_cast<std::size_t>(l)].data() + static_cast<std::size_t>(f) * (n + 1);
            for (std::uint32_t i = 0; i < n; ++i) dst[i] = static_cast<float>(x[i] * gain);
            dst[n] = dst[0];
        }
    }
    return w;
}

}  // namespace

int tableMultiplier(Table t) {
    switch (t) {
        case Table::Bell:
            return 8;
        case Table::Organ:
            return 2;
        default:
            return 1;
    }
}

Spectrum tableSpectrum(Table t, double p) {
    switch (t) {
        case Table::HarmonicRamp:
            return harmonicRamp(p);
        case Table::Pwm:
            return pwm(p);
        case Table::Formant:
            return formant(p);
        case Table::Bell:
            return bell(p);
        case Table::Organ:
            return organ(p);
        case Table::Digital:
            return digital(p);
        case Table::SpectralComb:
            return spectralComb(p);
    }
    return empty();
}

WavetableBank::WavetableBank() {
    for (int t = 0; t < kTableCount; ++t)
        tables_[static_cast<std::size_t>(t)] = build(static_cast<Table>(t));
}

const WavetableBank& WavetableBank::get() {
    static const WavetableBank bank;
    return bank;
}

}  // namespace supple::orrery
