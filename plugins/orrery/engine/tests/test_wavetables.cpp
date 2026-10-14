// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Wavetables (synth.md §4.2): generated, band-limited, alias-free, and the
// recipes do what their names say.
#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "../src/wavetables.h"
#include "orrery/engine.h"
#include "orrery/patch.h"
#include "supple/dsp/dsp.h"

using namespace supple;
using namespace supple::orrery;

namespace {

// |X_k| of a real signal (length n) normalized so a unit sine reads 1.
std::vector<double> magnitudes(const std::vector<double>& x) {
    supple::dsp::Fft fft;
    fft.prepare(x.size());
    std::vector<std::complex<double>> bins(x.size() / 2 + 1);
    fft.forward(x, bins);
    std::vector<double> m(bins.size());
    for (std::size_t k = 0; k < bins.size(); ++k)
        m[k] = std::abs(bins[k]) * 2.0 / static_cast<double>(x.size());
    return m;
}

Patch parse(std::string_view body) {
    static const PatchType type;
    auto p = type.parse(std::string(R"({"schema": "supple.orrery-patch/1.0", )") +
                        std::string(body) + "}");
    REQUIRE_MESSAGE(p, (p ? "" : p.error().toString()));
    return *p;
}

std::vector<double> renderNote(const Patch& p, int key, std::size_t frames,
                               std::uint32_t block = 256) {
    Engine engine;
    engine.prepare(48000.0, block);
    engine.setPatch(p);
    engine.reset();
    std::vector<float> l(frames), r(frames);
    Event on;
    on.type = Event::Type::NoteOn;
    on.key = static_cast<std::int16_t>(key);
    on.value = 1.0;
    for (std::size_t pos = 0; pos < frames; pos += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(block, frames - pos));
        if (pos == 0)
            engine.process(std::span<const Event>(&on, 1), Transport{}, l.data(), r.data(), n);
        else
            engine.process({}, Transport{}, l.data() + pos, r.data() + pos, n);
    }
    return std::vector<double>(l.begin(), l.end());
}

}  // namespace

TEST_CASE("every table: finite, normalized frames, and no energy above each level's harmonics") {
    const WavetableBank& bank = WavetableBank::get();
    for (int t = 0; t < kTableCount; ++t) {
        const Wavetable& w = bank.table(static_cast<Table>(t));
        INFO("table " << t);
        for (int f = 0; f < Wavetable::kFrames; f += 7) {
            for (int l = 0; l < Wavetable::kLevels; l += 3) {
                const std::uint32_t n = w.size[static_cast<std::size_t>(l)];
                const float* d = w.frame(l, f);
                std::vector<double> x(n);
                double peak = 0.0;
                for (std::uint32_t i = 0; i < n; ++i) {
                    REQUIRE(std::isfinite(d[i]));
                    x[i] = static_cast<double>(d[i]);
                    peak = std::max(peak, std::fabs(x[i]));
                }
                CHECK(d[n] == d[0]);  // the guard sample
                if (l == 0)
                    CHECK(peak == doctest::Approx(1.0).epsilon(1e-4));  // normalized full band
                else
                    CHECK(peak <= 1.25);  // fewer harmonics: Gibbs overshoot, same gain
                const auto m = magnitudes(x);
                const int limit = Wavetable::kTopHarmonics >> l;
                double above = 0.0;
                for (std::size_t k = static_cast<std::size_t>(limit) + 1; k < m.size(); ++k)
                    above = std::max(above, m[k]);
                CHECK(above < 1e-5);
            }
        }
    }
}

TEST_CASE("harmonic_ramp runs from a saw to a sine; bell partials are inharmonic") {
    const Spectrum saw = tableSpectrum(Table::HarmonicRamp, 0.0);
    CHECK(saw.amplitude[2] / saw.amplitude[1] == doctest::Approx(0.5));
    const Spectrum sine = tableSpectrum(Table::HarmonicRamp, 1.0);
    CHECK(sine.amplitude[2] / sine.amplitude[1] < 0.003);

    // A bell at A3 (220 Hz): the hum an octave below, the tierce near 1.25×.
    const Patch p =
        parse(R"("oscillators": {"osc1": {"type": "wavetable", "table": "bell", "position": 50}},
                             "envelopes": {"amp": {"attack": 0, "sustain": 100}})");
    const auto x = renderNote(p, 57, 16384);
    std::vector<double> tail(x.begin() + 4096, x.begin() + 4096 + 8192);
    const auto m = magnitudes(tail);
    auto bin = [&](double hz) {
        return m[static_cast<std::size_t>(std::lround(hz * 8192 / 48000.0))];
    };
    const double prime = bin(220.0);
    CHECK(prime > 0.01);
    CHECK(bin(110.0) > 0.1 * prime);   // hum
    CHECK(bin(275.0) > 0.1 * prime);   // tierce (1.2 → 10/8)
    CHECK(bin(330.0) > 0.05 * prime);  // quint
}

TEST_CASE("a high wavetable note stays free of aliases") {
    // A bright saw frame at ~2.1 kHz (key 96) in normal quality: every
    // component that is not a harmonic stays far below the fundamental.
    const Patch p = parse(R"("voice": {"quality": "normal"},
                             "oscillators": {"osc1": {"type": "wavetable", "table": "harmonic_ramp", "position": 0}},
                             "envelopes": {"amp": {"attack": 0, "sustain": 100}})");
    const auto x = renderNote(p, 96, 48000 + 16384);
    std::vector<double> w(x.begin() + 16384, x.begin() + 16384 + 32768);
    // 4-term Blackman-Harris (sidelobes ~ -92 dB): leakage must not pass for aliasing.
    for (std::size_t i = 0; i < w.size(); ++i) {
        const double ph =
            2.0 * 3.141592653589793 * static_cast<double>(i) / static_cast<double>(w.size());
        w[i] *= 0.35875 - 0.48829 * std::cos(ph) + 0.14128 * std::cos(2 * ph) -
                0.01168 * std::cos(3 * ph);
    }
    const auto m = magnitudes(w);
    const double f0 = 440.0 * std::exp2((96 - 69) / 12.0);
    const double binHz = 48000.0 / static_cast<double>(w.size());
    double fundamental = 0.0, worst = 0.0;
    for (std::size_t k = 1; k < m.size(); ++k) {
        const double hz = static_cast<double>(k) * binHz;
        const double h = hz / f0;
        const bool harmonic = std::fabs(h - std::round(h)) * f0 < 8.0 * binHz;
        if (std::fabs(hz - f0) < 3.0 * binHz)
            fundamental = std::max(fundamental, m[k]);
        else if (!harmonic && hz > 30.0)
            worst = std::max(worst, m[k]);
    }
    REQUIRE(fundamental > 0.0);
    const double db = 20.0 * std::log10(worst / fundamental);
    INFO("worst non-harmonic component: " << db << " dB");
    CHECK(db < -70.0);
}

TEST_CASE("wavetable renders do not depend on the block size; position morphs") {
    const Patch p =
        parse(R"("oscillators": {"osc1": {"type": "wavetable", "table": "formant", "position": 30}},
                             "lfos": {"lfo1": {"rate": 3}},
                             "routes": {"lfo1>osc1.position": {"amount": 40}})");
    const auto a = renderNote(p, 48, 12000, 256);
    const auto b = renderNote(p, 48, 12000, 37);
    double diff = 0.0, energy = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = std::max(diff, std::fabs(a[i] - b[i]));
        energy += a[i] * a[i];
    }
    CHECK(diff == 0.0);
    CHECK(energy > 1.0);
    const Patch q = parse(
        R"("oscillators": {"osc1": {"type": "wavetable", "table": "formant", "position": 90}})");
    const auto c = renderNote(q, 48, 12000);
    double change = 0.0;
    for (std::size_t i = 2000; i < c.size(); ++i) change += std::fabs(c[i] - a[i]);
    CHECK(change > 10.0);
}
