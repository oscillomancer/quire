// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Descriptors against synthetic signals with known answers (core-analysis §10).
#include <doctest/doctest.h>
#include <supple/base/environment.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <vector>

#include "supple/analysis/analysis.h"
#include "supple/base/rng.h"

using namespace supple;
using namespace supple::analysis;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

struct Audio {
    std::vector<float> l, r;
    std::vector<const float*> ptr;
    Signal signal(bool stereo = true) {
        ptr = stereo ? std::vector<const float*>{l.data(), r.data()}
                     : std::vector<const float*>{l.data()};
        return {ptr, static_cast<std::int64_t>(l.size()), kFs,
                stereo ? Layout::Stereo : Layout::Mono};
    }
};

Audio make(double seconds,
           const std::function<std::pair<double, double>(double t, std::size_t i)>& f) {
    Audio a;
    const auto n = static_cast<std::size_t>(seconds * kFs);
    a.l.resize(n);
    a.r.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto [x, y] = f(static_cast<double>(i) / kFs, i);
        a.l[i] = static_cast<float>(x);
        a.r[i] = static_cast<float>(y);
    }
    return a;
}

Audio sine(double hz, double amp, double seconds = 2.0) {
    return make(seconds, [&](double t, std::size_t) {
        const double v = amp * std::sin(2 * kPi * hz * t);
        return std::pair{v, v};
    });
}

// Band-limited sawtooth (additive): harmonic k at amplitude 1/k.
Audio saw(double hz, double amp, double seconds = 2.0, double stretch = 0.0) {
    return make(seconds, [&](double t, std::size_t) {
        double v = 0.0;
        for (int k = 1; k * hz * (1 + stretch) < 20000; ++k) {
            const double f = k * hz * (k > 1 ? 1.0 + stretch : 1.0);
            v += std::sin(2 * kPi * f * t) / k;
        }
        v *= amp * 0.6;
        return std::pair{v, v};
    });
}

Report run(Audio& a, bool stereo = true, Options o = {}) {
    auto r = analyze(a.signal(stereo), o);
    REQUIRE(r);
    return *r;
}

}  // namespace

TEST_CASE("sine: centroid, pitch, band placement") {
    Audio a = sine(1000.0, 0.5);
    const Report r = run(a);
    REQUIRE(r.spectral);
    CHECK(r.spectral->centroidHz.median == doctest::Approx(1000.0).epsilon(0.05));
    // A 0.5-amplitude sine sits at -6 dB in its 1/3-octave band (1 kHz is band 17).
    CHECK(r.spectral->ltasDb[17] == doctest::Approx(-6.02).epsilon(0.05));
    CHECK(r.spectral->bands[3].sharePct > 99.0);  // "mid" 500..2k
    REQUIRE(r.harmonic);
    CHECK(r.harmonic->pitched);
    CHECK(r.harmonic->f0Hz == doctest::Approx(1000.0).epsilon(0.002));
    CHECK(r.harmonic->note.rfind("B5+2", 0) == 0);  // 1000 Hz = B5 +21 cents
    REQUIRE(r.loudness);
    CHECK(r.loudness->samplePeak == doctest::Approx(-6.02).epsilon(0.01));
    REQUIRE(r.stereo);
    CHECK(r.stereo->width == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(r.stereo->correlation == doctest::Approx(1.0));
    CHECK(r.stereo->monoLossLu == doctest::Approx(0.0).epsilon(0.05));
}

TEST_CASE("sawtooth: harmonic ladder, f0, low inharmonicity") {
    Audio a = saw(220.0, 0.5);
    const Report r = run(a);
    REQUIRE(r.harmonic);
    REQUIRE(r.harmonic->pitched);
    CHECK(r.harmonic->f0Hz == doctest::Approx(220.0).epsilon(0.002));
    CHECK(r.harmonic->note == "A3");
    for (int k = 2; k <= 8; ++k) {
        CAPTURE(k);
        CHECK(r.harmonic->harmonicsDb[static_cast<std::size_t>(k - 1)] ==
              doctest::Approx(-20.0 * std::log10(k)).epsilon(0.08));
    }
    CHECK(r.harmonic->inharmonicity < 0.005);
    CHECK(r.harmonic->hnrDb > 20.0);
}

TEST_CASE("stretched partials report their inharmonicity") {
    Audio a = saw(200.0, 0.5, 2.0, 0.02);
    const Report r = run(a);
    REQUIRE(r.harmonic);
    REQUIRE(r.harmonic->pitched);
    CHECK(r.harmonic->inharmonicity == doctest::Approx(0.02).epsilon(0.3));
}

TEST_CASE("noise: flatness, tilt, stereo width, unpitched") {
    Rng rng(7);
    Audio white = make(3.0, [&](double, std::size_t) {
        return std::pair{0.3 * rng.uniform(-1.0, 1.0), 0.3 * rng.uniform(-1.0, 1.0)};
    });
    const Report r = run(white);
    REQUIRE(r.spectral);
    // Periodogram flatness of white noise is e^-γ ≈ 0.56, not 1.
    CHECK(r.spectral->flatness.median > 0.5);
    // Tilt is measured on 1/3-octave band power: white noise rises 3 dB/oct.
    CHECK(r.spectral->tiltDbPerOct == doctest::Approx(3.0).epsilon(0.1));
    REQUIRE(r.harmonic);
    CHECK_FALSE(r.harmonic->pitched);
    REQUIRE(r.stereo);
    CHECK(r.stereo->width == doctest::Approx(0.5).epsilon(0.03));
    CHECK(std::fabs(r.stereo->correlation) < 0.03);
    CHECK(r.stereo->monoLossLu == doctest::Approx(3.0).epsilon(0.1));
}

TEST_CASE("stereo: anti-phase and balance") {
    Audio anti = make(1.0, [](double t, std::size_t) {
        const double v = 0.4 * std::sin(2 * kPi * 440 * t);
        return std::pair{v, -v};
    });
    const Report r = run(anti);
    CHECK(r.stereo->width == doctest::Approx(1.0));
    CHECK(r.stereo->correlation == doctest::Approx(-1.0));
    Audio panned = make(1.0, [](double t, std::size_t) {
        const double v = 0.4 * std::sin(2 * kPi * 440 * t);
        return std::pair{v, 0.5 * v};
    });
    CHECK(run(panned).stereo->balanceDb == doctest::Approx(-6.02).epsilon(0.01));
}

TEST_CASE("hard-panned stereo balance keeps its infinite sign") {
    Options o;
    o.loudness = o.spectral = o.harmonic = o.envelope = false;
    Audio left = make(0.5, [](double t, std::size_t) {
        return std::pair{0.4 * std::sin(2 * kPi * 440.0 * t), 0.0};
    });
    const Report l = run(left, true, o);
    REQUIRE(l.stereo);
    CHECK(std::isinf(l.stereo->balanceDb));
    CHECK(l.stereo->balanceDb < 0.0);
    CHECK(toJson(l)["stereo"]["balance_db"].isNull());
    Audio right = make(0.5, [](double t, std::size_t) {
        return std::pair{0.0, 0.4 * std::sin(2 * kPi * 440.0 * t)};
    });
    const Report r = run(right, true, o);
    REQUIRE(r.stereo);
    CHECK(std::isinf(r.stereo->balanceDb));
    CHECK(r.stereo->balanceDb > 0.0);
}

TEST_CASE("5.0 mono downmix includes the left surround") {
    constexpr std::size_t n = 24000;
    std::array<std::vector<float>, 5> channels;
    for (auto& c : channels) c.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        channels[3][i] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * i / kFs));
    const float* ptr[5];
    for (std::size_t c = 0; c < 5; ++c) ptr[c] = channels[c].data();
    Signal s{ptr, static_cast<std::int64_t>(n), kFs, Layout::Surround50};
    Options o;
    o.loudness = o.harmonic = o.envelope = o.stereo = false;
    auto r = analyze(s, o);
    REQUIRE(r);
    REQUIRE(r->spectral);
    CHECK(r->spectral->centroidHz.median == doctest::Approx(1000.0).epsilon(0.05));
}

TEST_CASE("analysis rejects inconsistent layouts and invalid pitch ranges") {
    Audio a = sine(440.0, 0.5, 0.2);
    Options o;
    o.loudness = o.spectral = o.envelope = o.stereo = false;
    for (const auto range : {std::pair{400.0, 100.0}, std::pair{0.0, 4000.0},
                             std::pair{30.0, std::numeric_limits<double>::infinity()}}) {
        o.f0RangeHz = range;
        CHECK_FALSE(analyze(a.signal(), o));
    }
    o.harmonic = false;
    a.ptr = {a.l.data()};
    Signal wrong{a.ptr, static_cast<std::int64_t>(a.l.size()), kFs, Layout::Stereo};
    CHECK_FALSE(analyze(wrong, o));
}

TEST_CASE("envelope: attack, decay20, release with extrapolation") {
    // 20 ms linear attack, then exponential decay with τ = 100 ms.
    Audio pluck = make(1.5, [](double t, std::size_t) {
        const double env = t < 0.02 ? t / 0.02 : std::exp(-(t - 0.02) / 0.1);
        const double v = 0.8 * env * std::sin(2 * kPi * 330 * t);
        return std::pair{v, v};
    });
    Options o;
    o.noteOffSeconds = 0.3;
    const Report r = run(pluck, true, o);
    REQUIRE(r.envelope);
    CHECK(r.envelope->attackMs == doctest::Approx(16.0).epsilon(0.25));  // 10 %..90 % of 20 ms
    REQUIRE(r.envelope->decay20Ms);
    CHECK(*r.envelope->decay20Ms == doctest::Approx(100.0 * std::log(10.0)).epsilon(0.05));
    REQUIRE(r.envelope->releaseMs);
    CHECK(*r.envelope->releaseMs == doctest::Approx(100.0 * std::log(1000.0)).epsilon(0.05));
    // A signal that ends before -60 dB extrapolates.
    Audio cut = make(0.6, [](double t, std::size_t) {
        const double v = 0.8 * std::exp(-t / 0.1) * std::sin(2 * kPi * 330 * t);
        return std::pair{v, v};
    });
    o.noteOffSeconds = 0.0;
    const Report c = run(cut, true, o);
    REQUIRE(c.envelope->releaseMs);
    CHECK(c.envelope->releaseExtrapolated);
    CHECK(*c.envelope->releaseMs == doctest::Approx(690.8).epsilon(0.1));
}

TEST_CASE("onsets: a click train at 4 per second") {
    Audio clicks = make(3.0, [](double t, std::size_t) {
        const double ph = std::fmod(t, 0.25);
        const double v =
            ph < 0.03 ? 0.7 * std::exp(-ph / 0.005) * std::sin(2 * kPi * 2000 * t) : 0.0;
        return std::pair{v, v};
    });
    const Report r = run(clicks);
    REQUIRE(r.envelope);
    CHECK(r.envelope->onsetsPerSecond == doctest::Approx(4.0).epsilon(0.1));
}

TEST_CASE("compare: a gain change is loud, not timbral; a filter change is timbral") {
    Audio a = saw(110.0, 0.3), b = saw(110.0, 0.6);
    const Report ra = run(a), rb = run(b);
    const Comparison c = compare(ra, rb);
    CHECK(c.ltasRmsDb < 0.3);
    bool loudFlag = false;
    for (const auto& d : c.deltas)
        if (d.key == "loudness.integrated_lufs")
            loudFlag = d.notable && d.delta == doctest::Approx(6.02).epsilon(0.02);
    CHECK(loudFlag);
    CHECK(c.deltas.front().notable);
    Audio dark = saw(110.0, 0.3, 2.0);
    // Crude low-pass: one-pole smoothing.
    double y = 0.0;
    for (auto& v : dark.l) v = static_cast<float>(y += 0.05 * (static_cast<double>(v) - y));
    dark.r = dark.l;
    const Comparison c2 = compare(ra, run(dark));
    CHECK(c2.ltasRmsDb > 3.0);
    bool centroidFlag = false;
    for (const auto& d : c2.deltas)
        if (d.key == "spectral.centroid_hz.median") centroidFlag = d.notable && d.delta < 0.0;
    CHECK(centroidFlag);
    CHECK(summarize(c2).find("Timbre difference") != std::string::npos);
}

TEST_CASE("compare applies descriptor thresholds to each statistic") {
    Report a, b;
    a.spectral.emplace();
    b.spectral.emplace();
    a.spectral->centroidHz.mean = 1000.0;
    b.spectral->centroidHz.mean = 1200.0;
    a.spectral->flatness.p90 = 0.2;
    b.spectral->flatness.p90 = 0.3;
    a.stereo.emplace();
    b.stereo.emplace();
    a.stereo->correlationMean = 0.1;
    b.stereo->correlationMean = 0.3;
    const auto c = compare(a, b);
    for (const auto& key :
         {"spectral.centroid_hz.mean", "spectral.flatness.p90", "stereo.correlation_mean"}) {
        bool found = false;
        for (const auto& delta : c.deltas)
            if (delta.key == key) found = delta.notable;
        CHECK(found);
    }
}

TEST_CASE("profiles: targets, ranges, LTAS, unknown keys") {
    Audio a = sine(1000.0, 0.5);
    const Report r = run(a);
    auto profile = json::parse(R"({"schema": "supple.analysis-profile/1.0",
        "targets": {"spectral.centroid_hz.median": {"target": 1100, "tolerance": 200},
                    "loudness.true_peak_dbtp": {"max": -10, "weight": 2}}})");
    REQUIRE(profile);
    auto e = evaluate(r, *profile);
    REQUIRE(e);
    REQUIRE(e->targets.size() == 2);
    CHECK(e->targets[0].pass);
    CHECK_FALSE(e->targets[1].pass);  // -6 dBTP is above the -10 max
    CHECK_FALSE(e->pass);
    CHECK(e->loss > 0.0);
    auto bad = json::parse(
        R"({"schema": "supple.analysis-profile/1.0", "targets": {"spectral.centroid": {"target": 1, "tolerance": 1}}})");
    auto err = evaluate(r, *bad);
    REQUIRE_FALSE(err);
    CHECK(err.error().path == "/targets/spectral.centroid");
    CHECK(err.error().hint.find("spectral.centroid_hz") != std::string::npos);
}

TEST_CASE("profiles reject invalid weights and bounds") {
    Report r;
    r.spectral.emplace();
    r.spectral->centroidHz.median = 1000.0;
    for (const char* target : {R"({"target": 2000, "tolerance": 100, "weight": -1})",
                               R"({"target": 2000, "tolerance": 100, "weight": 0})",
                               R"({"min": 2000, "max": 1000})", R"({"min": "100", "max": 2000})"}) {
        const std::string source =
            std::string(
                R"({"schema":"supple.analysis-profile/1.0","targets":{"spectral.centroid_hz.median":)") +
            target + "}}";
        auto profile = json::parse(source);
        REQUIRE(profile);
        auto result = evaluate(r, *profile);
        CHECK_FALSE(result);
    }
    std::string ltas = R"({"schema":"supple.analysis-profile/1.0","ltas":["bad")";
    for (int i = 1; i < kThirdOctaves; ++i) ltas += ",0";
    ltas += "]}";
    auto profile = json::parse(ltas);
    REQUIRE(profile);
    CHECK_FALSE(evaluate(r, *profile));
}

TEST_CASE("masking: a loud tone masks a quiet neighbour, not a distant one") {
    Audio masker = sine(1000.0, 0.5);
    Audio near = sine(1050.0, 0.005), far = sine(5000.0, 0.005);
    auto m1 = masking(masker.signal(), near.signal());
    REQUIRE(m1);
    CHECK(m1->maskedEnergyPct > 90.0);
    REQUIRE_FALSE(m1->worst.empty());
    CHECK(m1->bands[static_cast<std::size_t>(m1->worst[0])].lowHz == 920.0);
    auto m2 = masking(masker.signal(), far.signal());
    REQUIRE(m2);
    CHECK(m2->maskedEnergyPct < 10.0);
}

TEST_CASE("masking uses surround channels in its mono downmix") {
    constexpr std::size_t n = 24000;
    std::array<std::vector<float>, 5> channels;
    for (auto& c : channels) c.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        channels[3][i] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * i / kFs));
    const float* ptr[5];
    for (std::size_t c = 0; c < 5; ++c) ptr[c] = channels[c].data();
    Signal surround{ptr, static_cast<std::int64_t>(n), kFs, Layout::Surround50};
    Audio quiet = sine(1000.0, 0.001, 0.5);
    auto m = masking(surround, quiet.signal(false));
    REQUIRE(m);
    CHECK(m->maskedEnergyPct > 90.0);
}

TEST_CASE("report JSON: summary is compact, deterministic, and rounded") {
    // A 4 s synth-like stereo render: saw notes with a decaying filter-ish envelope.
    Rng rng(3);
    Audio a = make(4.0, [&](double t, std::size_t) {
        const double note = std::floor(t / 0.5);
        const double hz = 110.0 * std::pow(2.0, std::fmod(note * 5, 12) / 12.0);
        const double ph = std::fmod(t, 0.5);
        double v = 0.0;
        for (int k = 1; k < 20; ++k)
            v += std::sin(2 * kPi * k * hz * t) / k * std::exp(-ph * k * 2.0);
        v *= 0.3 * std::exp(-ph * 3.0);
        const double n = 0.01 * rng.uniform(-1.0, 1.0);
        return std::pair{v + n, 0.9 * v - n};
    });
    const Report r = run(a);
    const std::string summary = json::writeCompact(toJson(r, Detail::Summary));
    INFO(summary);
    CHECK(summary.size() <= 1600);
    CHECK(json::writeCompact(toJson(*analyze(a.signal()), Detail::Summary)) == summary);
    const std::string full = json::writeCompact(toJson(r, Detail::Full));
    CHECK(full.size() > summary.size());
    const std::string prose = summarize(r);
    std::istringstream words(prose);
    int count = 0;
    for (std::string w; words >> w;) ++count;
    CHECK(count <= 80);
}

TEST_CASE("example reports (docs/specs/examples/analysis)") {
    // Pluck, pad and drum loop from simple synthesis; the committed reports
    // must match (set SUPPLE_UPDATE_EXAMPLES=1 to rewrite them).
    Rng rng(11);
    std::vector<std::pair<std::string, Audio>> examples;
    examples.emplace_back("pluck", make(2.0, [](double t, std::size_t) {
                              double v = 0.0;
                              for (int k = 1; k < 24; ++k)
                                  v += std::sin(2 * kPi * k * 196.0 * t) / k *
                                       std::exp(-t * (3.0 + k * 1.5));
                              v *= 0.5;
                              return std::pair{v, v};
                          }));
    examples.emplace_back("pad", make(4.0, [](double t, std::size_t) {
                              // Four notes, each three unison voices (-7, 0, +7 ct) with
                              // per-channel phases.
                              double l = 0.0, r = 0.0;
                              int voice = 0;
                              for (double hz : {130.81, 164.81, 196.0, 246.94}) {
                                  for (double ct : {-7.0, 0.0, 7.0}) {
                                      const double f = hz * std::pow(2.0, ct / 1200.0);
                                      const double pl = 0.37 * voice, pr = 0.91 * voice + 1.3;
                                      for (int k = 1; k < 8; ++k) {
                                          l += std::sin(2 * kPi * k * f * t + k * pl) / (k * k);
                                          r += std::sin(2 * kPi * k * f * t + k * pr) / (k * k);
                                      }
                                      ++voice;
                                  }
                              }
                              const double env = std::min(1.0, t / 1.5);
                              return std::pair{0.05 * env * l, 0.05 * env * r};
                          }));
    examples.emplace_back(
        "drums", make(4.0, [&](double t, std::size_t) {
            const double beat = std::fmod(t, 0.5), off = std::fmod(t + 0.25, 0.5);
            const double kick = std::exp(-beat / 0.08) *
                                std::sin(2 * kPi * (50.0 + 80.0 * std::exp(-beat / 0.02)) * beat);
            const double hat = off < 0.05 ? std::exp(-off / 0.01) * rng.uniform(-1.0, 1.0) : 0.0;
            const double v = 0.7 * kick + 0.25 * hat;
            return std::pair{v, v};
        }));
    const std::filesystem::path dir =
        std::filesystem::path(SUPPLE_SOURCE_DIR) / "docs/specs/examples/analysis";
    const bool update = supple::environmentFlagEnabled("SUPPLE_UPDATE_EXAMPLES");
    for (auto& [name, audio] : examples) {
        CAPTURE(name);
        const std::string text = json::writeCanonical(toJson(run(audio), Detail::Standard)) + "\n";
        const auto path = dir / (name + ".json");
        if (update) {
            std::filesystem::create_directories(dir);
            std::ofstream(path) << text;
            continue;
        }
        std::ifstream in(path);
        REQUIRE_MESSAGE(in.good(),
                        "missing " << path.string() << " (run with SUPPLE_UPDATE_EXAMPLES=1)");
        std::stringstream ss;
        ss << in.rdbuf();
        CHECK(ss.str() == text);
    }
}

TEST_CASE("analysis rejects invalid note-off times and ignores times beyond the signal") {
    auto audio = sine(1000, 0.1, 0.02);
    Options options;
    options.loudness = options.spectral = options.harmonic = options.stereo = false;
    for (double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        options.noteOffSeconds = invalid;
        auto result = analyze(audio.signal(), options);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "analysis.note_off");
    }
    options.noteOffSeconds = std::numeric_limits<double>::max();
    auto result = analyze(audio.signal(), options);
    REQUIRE(result);
    CHECK_FALSE(result->envelope->releaseMs);
}
TEST_CASE("spectral tilt comparison reports dB per octave") {
    Report a, b;
    a.spectral.emplace();
    b.spectral.emplace();
    b.spectral->tiltDbPerOct = 2;
    const auto comparison = compare(a, b);
    auto found =
        std::find_if(comparison.deltas.begin(), comparison.deltas.end(),
                     [](const auto& delta) { return delta.key == "spectral.tilt_db_per_oct"; });
    REQUIRE(found != comparison.deltas.end());
    CHECK(found->unit == "dB/oct");
}

TEST_CASE("offline analysis and masking reject null channels and nonfinite audio") {
    float sample = 0;
    const float* valid[] = {&sample};
    const float* missing[] = {nullptr};
    const Signal good{valid, 1, kFs, Layout::Mono};
    const Signal bad{missing, 1, kFs, Layout::Mono};
    CHECK_FALSE(analyze(bad));
    CHECK_FALSE(masking(bad, good));
    CHECK_FALSE(masking(good, bad));
    for (float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        sample = invalid;
        CHECK_FALSE(analyze(good));
        CHECK_FALSE(masking(good, good));
    }
}

TEST_CASE("offline true peak includes the final interpolation filter response") {
    Options options;
    options.spectral = options.harmonic = options.envelope = options.stereo = false;
    auto audio = make(0.01, [](double, std::size_t) { return std::pair{0.0, 0.0}; });
    for (std::size_t i = audio.l.size() - 8; i < audio.l.size(); ++i)
        audio.l[i] = audio.r[i] = (i % 4 < 2 ? 0.9f : -0.9f);
    auto shortReport = analyze(audio.signal(), options);
    REQUIRE(shortReport);
    audio.l.resize(audio.l.size() + 32, 0);
    audio.r.resize(audio.r.size() + 32, 0);
    auto padded = analyze(audio.signal(), options);
    REQUIRE(padded);
    CHECK(shortReport->loudness->truePeak ==
          doctest::Approx(padded->loudness->truePeak).epsilon(1e-10));
}

TEST_CASE("LTAS compares the union of active bands, including silence") {
    Report silent, signal;
    silent.spectral.emplace();
    signal.spectral.emplace();
    silent.spectral->ltasDb.fill(-120);
    signal.spectral->ltasDb.fill(-120);
    signal.spectral->ltasDb[10] = -20;
    CHECK(compare(silent, signal).ltasRmsDb == doctest::Approx(80));
    CHECK(compare(signal, silent).ltasRmsDb == doctest::Approx(80));
    CHECK(compare(silent, silent).ltasRmsDb == 0);
    signal.spectral->ltasDb.fill(-20);
    CHECK(compare(silent, signal).ltasRmsDb == doctest::Approx(80));
}

TEST_CASE("profile weighted means stay finite with very large valid weights") {
    Report report;
    report.spectral.emplace();
    report.spectral->centroidHz.median = 1000;
    report.spectral->rolloffHz.median = 2000;
    auto profile = json::parse(R"({"schema":"supple.analysis-profile/1.0","targets":{
        "spectral.centroid_hz.median":{"target":800,"tolerance":100,"weight":1e308},
        "spectral.rolloff_hz.median":{"target":1700,"tolerance":100,"weight":1e308}}})");
    REQUIRE(profile);
    auto result = evaluate(report, *profile);
    REQUIRE(result);
    CHECK(result->loss == doctest::Approx(2.5));
}

TEST_CASE("R20 short audible buffers are not summarized as silence") {
    Audio audible = sine(440, 0.5, 0.1);
    auto report = run(audible);
    REQUIRE(report.loudness);
    CHECK_FALSE(std::isfinite(report.loudness->integrated));
    CHECK(summarize(report).find("Too short to measure loudness") != std::string::npos);
    CHECK(summarize(report).find("Silent") == std::string::npos);
    for (double seconds : {0.1, 1.0}) {
        Audio silent = sine(440, 0, seconds);
        CHECK(summarize(run(silent)).find("Silent") != std::string::npos);
    }
}

TEST_CASE("a constant (DC) signal has no pitch") {
    Audio dc = make(1.0, [](double, std::size_t) { return std::pair{0.25, 0.25}; });
    const Report r = run(dc);
    REQUIRE(r.harmonic);
    CHECK_FALSE(r.harmonic->pitched);
}

TEST_CASE("mono loss: full cancellation is large, unmeasurable stereo is unavailable") {
    Audio anti = make(2.0, [](double t, std::size_t) {
        const double v = 0.4 * std::sin(2 * kPi * 440 * t);
        return std::pair{v, -v};
    });
    const Report r = run(anti);
    REQUIRE(r.stereo);
    CHECK(r.stereo->monoLossLu > 40.0);  // the downmix is silent: against the -70 LUFS gate
    Audio brief = make(0.2, [](double t, std::size_t) {
        const double v = 0.4 * std::sin(2 * kPi * 440 * t);
        return std::pair{v, v};
    });
    const Report b = run(brief);
    REQUIRE(b.stereo);
    CHECK(std::isnan(b.stereo->monoLossLu));
    CHECK(flatten(b).count("stereo.mono_loss_lu") == 0);
}
