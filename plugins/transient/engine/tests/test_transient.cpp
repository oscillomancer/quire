// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <cstdio>
#include <numbers>
#include "supple/base/rng.h"
#include "transient/processor.h"
using namespace supple;
using namespace supple::transient;
namespace {
using Audio = std::array<std::vector<double>, 2>;
Audio silence(std::size_t n) { return {std::vector<double>(n), std::vector<double>(n)}; }
Audio sine(double sr, double seconds, double amp, double hz) {
    auto a = silence(static_cast<std::size_t>(sr * seconds));
    for (std::size_t i = 0; i < a[0].size(); ++i)
        a[0][i] = a[1][i] = amp * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / sr);
    return a;
}
Audio hit(double sr, double amp = .1) {
    auto a = sine(sr, .2, amp, 1000);
    for (std::size_t i = 0; i < a[0].size(); ++i)
        a[0][i] = a[1][i] =
            a[0][i] * std::exp(-std::max(0., static_cast<double>(i) / sr - .005) / .02);
    return a;
}
struct Event {
    std::size_t time, id;
    double value;
};
Audio render(const Audio& a, Values v, double sr = 48000, std::size_t block = 64,
             std::span<const Event> events = {}) {
    Processor p;
    REQUIRE(p.prepare(sr, 4096));
    for (std::size_t i = 0; i < kInputLevel; ++i) p.setParam(i, v[i]);
    auto out = silence(a[0].size());
    std::size_t pos = 0, e = 0;
    while (pos < a[0].size()) {
        while (e < events.size() && events[e].time == pos) {
            p.setParam(events[e].id, events[e].value);
            ++e;
        }
        auto n = std::min(block, a[0].size() - pos);
        if (e < events.size()) n = std::min(n, events[e].time - pos);
        const double* in[]{a[0].data() + pos, a[1].data() + pos};
        double* op[]{out[0].data() + pos, out[1].data() + pos};
        p.process(in, op, static_cast<std::uint32_t>(n));
        pos += n;
    }
    return out;
}
double rms(const std::vector<double>& a, std::size_t start, std::size_t end) {
    double sum = 0;
    for (auto i = start; i < end; ++i) sum += a[i] * a[i];
    return std::sqrt(sum / static_cast<double>(end - start));
}
double db(double x) { return 20 * std::log10(std::max(x, 1e-300)); }
double change(const Audio& a, const Audio& b, double sr, double start, double end) {
    const auto s = static_cast<std::size_t>(sr * start), e = static_cast<std::size_t>(sr * end);
    return db(rms(b[0], s, e) / rms(a[0], s, e));
}
double component(const std::vector<double>& a, double sr, double hz, std::size_t start) {
    double re = 0, im = 0;
    for (auto i = start; i < a.size(); ++i) {
        const auto phase = 2 * std::numbers::pi * hz * static_cast<double>(i) / sr;
        re += a[i] * std::cos(phase);
        im += a[i] * std::sin(phase);
    }
    return 2 * std::hypot(re, im) / static_cast<double>(a.size() - start);
}
}  // namespace
TEST_CASE("transient table, patch round trips, merge errors and describe") {
    REQUIRE(params::validateTable(table()));
    CHECK(table().size() == kNumParams);
    for (std::size_t i = 0; i < kNumParams; ++i) {
        CHECK(table()[i].id == i);
        if (i < kInputLevel && table()[i].type == params::Type::Float)
            CHECK(table()[i].smoothingMs == 20);
    }
    CHECK((table()[kClip].flags & params::kAutomatable) == 0);
    CHECK_FALSE(patchType().codec().hasSeed());
    CHECK(patchType().schemaId() == "supple.transient-patch/1.0");
    auto p = patchType().defaults();
    auto text = patchType().serialize(p);
    auto parsed = patchType().parse(text);
    REQUIRE(parsed);
    CHECK(patchType().serialize(*parsed) == text);
    for (std::size_t i = 0; i < kInputLevel; ++i)
        p.body.values[i] = table()[i].max == table()[i].def ? table()[i].min : table()[i].max;
    text = patchType().serialize(p);
    parsed = patchType().parse(text);
    REQUIRE(parsed);
    CHECK(patchType().serialize(*parsed) == text);
    auto j = json::parse(R"({"params":{"speed":100,"unknown":1}})");
    REQUIRE(j);
    auto bad = patchType().merge(p, *j);
    REQUIRE_FALSE(bad);
    CHECK_FALSE((bad.error().path.empty() && bad.error().details.empty()));
    j = json::parse(R"({"params":{"attack":"60 %","sustain":-30}})");
    REQUIRE(j);
    auto merge = patchType().merge(p, *j);
    REQUIRE(merge);
    CHECK(merge->body.values[kAttack] == 60);
    CHECK(describe(defaults()).empty());
    auto v = defaults();
    v[kSpeed] = 20;
    v[kOutput] = -3;
    CHECK(describe(v).find("ms") != std::string::npos);
    CHECK(describe(v).find("dB") != std::string::npos);
    CHECK(describe(v).size() <= 300);
}
TEST_CASE("transient default bit-exact at all rates, blocks, mono, float and double") {
    for (double sr : {22050., 44100., 48000., 88200., 96000., 192000., 384000.}) {
        CAPTURE(sr);
        auto a = sine(sr, .02, .3, 997);
        a[0][0] = -0.;
        a[0][1] = std::numeric_limits<double>::denorm_min();
        for (auto block : {1u, 7u, 64u, 4096u}) {
            auto out = render(a, defaults(), sr, block);
            CHECK(out == a);
            CHECK(std::signbit(out[0][0]));
        }
        Processor p;
        REQUIRE(p.prepare(sr, 64, 1));
        float a32[64], b[64];
        for (int i = 0; i < 64; ++i) a32[i] = static_cast<float>(std::sin(i));
        a32[0] = std::numeric_limits<float>::denorm_min();
        const float* in[]{a32};
        float* out[]{b};
        p.process(in, out, 64);
        for (int i = 0; i < 64; ++i) CHECK(b[i] == a32[i]);
    }
}
TEST_CASE("transient onset, sustain, level independence and steady sine") {
    for (double sr : {44100., 48000., 88200., 96000., 192000.}) {
        CAPTURE(sr);
        const auto a = hit(sr);
        auto v = defaults();
        v[kAttack] = 100;
        auto attack = render(a, v, sr);
        const double onset = change(a, attack, sr, 0, .005), tail = change(a, attack, sr, .08, .1);
        CHECK(onset >= 6);
        CHECK(std::abs(tail) < 1);
        v = defaults();
        v[kSustain] = -100;
        auto sustain = render(a, v, sr);
        const double sustainOnset = change(a, sustain, sr, 0, .005),
                     sustainTail = change(a, sustain, sr, .08, .1);
        CHECK(std::abs(sustainOnset) < 1);
        CHECK(sustainTail <= -6);
        v[kAttack] = 100;
        auto louder = hit(sr, .1 * std::pow(10., .5));
        auto shaped = render(a, v, sr);
        auto loudShaped = render(louder, v, sr);
        double diff = 0;
        for (std::size_t i = 0; i < a[0].size(); ++i)
            if (std::abs(a[0][i]) > 1e-8)
                diff = std::max(diff, std::abs(db(std::abs(shaped[0][i] / a[0][i])) -
                                               db(std::abs(loudShaped[0][i] / louder[0][i]))));
        CHECK(diff < .5);
        double steadyGain = 0;
        for (double hz : {20., 100., 1000., 5000.}) {
            const auto steady = sine(sr, 1.2, .2, hz);
            auto steadyOut = render(steady, v, sr);
            const double g = change(steady, steadyOut, sr, 1., 1.2);
            CHECK(std::abs(g) < .1);
            steadyGain = std::max(steadyGain, std::abs(g));
        }
        std::printf(
            "%s %.0f Hz: attack onset %.6f dB, tail %.6f; sustain onset %.6f, tail %.6f; level "
            "diff %.9f dB; steady %.6f dB\n",
            kName, sr, onset, tail, sustainOnset, sustainTail, diff, steadyGain);
    }
}
TEST_CASE("transient sample events and clipped audio are block invariant") {
    auto a = hit(48000);
    for (std::size_t i = 0; i < a[0].size(); ++i) a[1][i] = .37 * a[0][i];
    const Event events[] = {{1, kAttack, 100},      {713, kSpeed, 1},   {1279, kSensitivity, 100},
                            {2013, kSustain, -100}, {7001, kOutput, 6}, {8113, kMix, 43}};
    for (double clip : {0., 1.}) {
        auto v = defaults();
        v[kClip] = clip;
        auto reference = render(a, v, 48000, 1, events);
        for (auto block : {7u, 64u, 4096u}) CHECK(render(a, v, 48000, block, events) == reference);
    }
}
TEST_CASE("transient stereo link and sample rate timing") {
    for (double sr : {44100., 48000., 96000., 192000.}) {
        auto a = hit(sr);
        for (auto& x : a[1]) x *= .25;
        auto v = defaults();
        v[kAttack] = 100;
        v[kSustain] = -100;
        auto b = render(a, v, sr);
        for (std::size_t i = 0; i < a[0].size(); ++i) CHECK(b[1][i] == b[0][i] * .25);
        // A held DC step saturates the onset target: the 1 ms gain pole reaches
        // 63% of 12 dB, allowing the detector's short rise time.
        Processor p;
        REQUIRE(p.prepare(sr, 1));
        p.setParam(kAttack, 100);
        double x = .2, y = 0;
        const double* in[]{&x, &x};
        double* out[]{&y, &y};
        for (int i = 0; i < static_cast<int>(std::lround(sr * .001)); ++i) p.process(in, out, 1);
        CHECK(p.shapingGain() == doctest::Approx(12 * (1 - std::exp(-1.))).epsilon(.02));
    }
}
TEST_CASE("transient clip exact latency, dry alignment, reset and passband") {
    for (double sr : {44100., 48000., 96000., 192000.}) {
        CAPTURE(sr);
        auto v = defaults();
        v[kClip] = 1;
        Processor p;
        REQUIRE(p.prepare(sr, 4096));
        CHECK(p.latency() == 0);
        p.setParam(kClip, 1);
        dsp::Oversampler os;
        os.prepare(2);
        CHECK(p.latency() == os.latency());
        CHECK(p.latency() == 72);
        const auto delay = p.latency();
        auto a = silence(1024);
        a[0][0] = a[1][0] = 1e-5;
        auto b = render(a, v, sr);
        auto it = std::max_element(b[0].begin(), b[0].end());
        CHECK(static_cast<std::size_t>(it - b[0].begin()) == delay);
        // Reset must clear both histories while retaining prepared FIR coefficients.
        auto resetOut = silence(a[0].size());
        const double* in[]{a[0].data(), a[1].data()};
        double* out[]{resetOut[0].data(), resetOut[1].data()};
        p.process(in, out, static_cast<std::uint32_t>(a[0].size()));
        CHECK(resetOut == b);
        p.reset();
        p.process(in, out, static_cast<std::uint32_t>(a[0].size()));
        CHECK(resetOut == b);
        v[kMix] = 0;
        a = sine(sr, .04, .3, 997);
        for (auto& sample : a[1]) sample *= -.37;
        b = render(a, v, sr);
        for (std::size_t c = 0; c < a.size(); ++c)
            for (std::size_t i = 0; i < b[c].size(); ++i)
                CHECK(b[c][i] == (i < delay ? 0 : a[c][i - delay]));
        v[kMix] = 100;
        double worst = 0;
        for (double ratio : {.01, .1, .2, .3, .4, .45}) {
            a = sine(sr, .04, 1e-5, sr * ratio);
            b = render(a, v, sr);
            const double error = db(rms(b[0], 1024, b[0].size() - 256) /
                                    rms(a[0], 1024 - delay, a[0].size() - 256 - delay));
            CHECK(std::abs(error) < .01);
            worst = std::max(worst, std::abs(error));
        }
        std::printf("%s clip %.0f Hz: %u samples, stereo dry exact, worst passband %.6f dB\n",
                    kName, sr, delay, worst);
        a = sine(sr, .05, 3, sr * .1);
        b = render(a, v, sr);
        for (auto x : b[0]) CHECK(std::isfinite(x));
        p.setParam(kClip, 0);
        CHECK(p.latency() == 0);
    }
}
TEST_CASE("transient clip suppresses folded harmonics versus base-rate tanh") {
    auto a = sine(48000, .1, 3, 5000);
    auto v = defaults();
    v[kClip] = 1;
    auto b = render(a, v);
    auto naive = a;
    for (auto& x : naive[0]) x = std::tanh(x);
    // At 48 kHz, harmonics 7/9/11 (35/45/55 kHz) fold to 13/3/7 kHz at 1x.
    // The fifth folds to 23 kHz, outside the audible band we protect.
    for (const int harmonic : {7, 9, 11}) {
        const double fold = std::abs(std::remainder(harmonic * 5000.0, 48000.0));
        const double alias = component(b[0], 48000, fold, 2400),
                     base = component(naive[0], 48000, fold, 2400);
        CAPTURE(harmonic);
        CAPTURE(fold);
        std::printf(
            "%s clip 5 kHz, amplitude 3 peak, 48 kHz: harmonic %d folds to %.0f Hz, "
            "2x %.3f dBFS (1x %.3f), improvement %.3f dB\n",
            kName, harmonic, fold, db(alias), db(base), db(base / alias));
        CHECK(db(alias / base) < -20);
    }
}
TEST_CASE("transient automation stress, silence recovery and repeatable saved state") {
    Rng rng(73);
    Processor p;
    REQUIRE(p.prepare(48000, 64));
    p.setParam(kClip, 1);
    double a[64], b[64], l[64], r[64];
    const double* in[]{a, b};
    double* out[]{l, r};
    std::uint32_t remaining = 240000;
    while (remaining) {
        const auto n = std::min(remaining, 1u + static_cast<unsigned>(rng.next() % 64));
        remaining -= n;
        for (std::size_t i = 0; i < kInputLevel; ++i) {
            const auto& spec = table()[i];
            p.setParam(i, spec.min + (spec.max - spec.min) * (rng.uniform()));
        }
        for (unsigned i = 0; i < n; ++i) {
            a[i] = (rng.uniform() - .5) * .8;
            b[i] = a[i] * -.37;
        }
        p.process(in, out, n);
        for (unsigned i = 0; i < n; ++i) {
            REQUIRE(std::isfinite(l[i]));
            REQUIRE(std::abs(l[i]) < 100);
            REQUIRE(std::isfinite(r[i]));
            REQUIRE(std::abs(r[i]) < 100);
        }
    }
    for (std::size_t i = 0; i < kInputLevel; ++i) p.setParam(i, defaults()[i]);
    std::fill_n(a, 64, 0.);
    std::fill_n(b, 64, 0.);
    for (int i = 0; i < 2000; ++i) p.process(in, out, 64);
    for (auto x : l) CHECK(x == 0);
    for (int k = 0; k < 8; ++k) {
        auto patch = patchType().defaults();
        for (std::size_t i = 0; i < kInputLevel; ++i) {
            const auto& spec = table()[i];
            const double v = spec.min + (spec.max - spec.min) * (rng.uniform());
            patch.body.values[i] = i == kClip ? (v > .5 ? 1 : 0) : v;
        }
        auto parsed = patchType().parse(patchType().serialize(patch));
        REQUIRE(parsed);
        Values aValues{}, bValues{};
        std::copy(patch.body.values.begin(), patch.body.values.end(), aValues.begin());
        std::copy(parsed->body.values.begin(), parsed->body.values.end(), bValues.begin());
        auto source = hit(48000);
        CHECK(render(source, aValues) == render(source, bValues));
    }
    CHECK_FALSE(p.prepare(0, 64));
    CHECK_FALSE(p.prepare(48000, 0));
    CHECK_FALSE(p.prepare(48000, 64, 3));
    p.setParam(kAttack, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(p.prepare(1234.57, 64));
}
