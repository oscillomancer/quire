// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "playrate_dsp.h"
#include "supple/dsp/dsp.h"

using namespace supple::engine::detail;

namespace {
struct Source {
    double frequency = .01;
    bool constant = false;
    static float read(const void* context, int channel, std::int64_t frame) noexcept {
        const auto& s = *static_cast<const Source*>(context);
        return (channel == 0 ? 1.f : -1.f) *
               (s.constant ? .25f
                           : static_cast<float>(.4 * std::sin(2 * std::numbers::pi * s.frequency *
                                                              double(frame))));
    }
    RateSource reader() const { return {this, read}; }
};

double square(const void*, double sample) noexcept { return sample < 100 ? .25 : 4; }

void clockRun(PlayrateClock& clock, std::span<double> output) SUPPLE_RT { clock.advance(output); }

void stretchRun(StretchReader& reader, RateSource source, double rate, std::span<float> left,
                std::span<float> right) SUPPLE_RT {
    supple::dsp::DenormalGuard denormals;
    while (!left.empty()) {
        const bool ok =
            reader.process(source, reader.inputPosition() + rate * kRateQuantum,
                           left.subspan<0, kRateQuantum>(), right.subspan<0, kRateQuantum>());
        // No doctest/logging inside the RT annotation.
        if (!ok) {
            left[0] = std::numeric_limits<float>::quiet_NaN();
            return;
        }
        left = left.subspan(kRateQuantum);
        right = right.subspan(kRateQuantum);
    }
}

void start(StretchReader& reader, RateSource source, double rate, std::uint32_t budget) {
    reader.seek(source, 0, rate);
    for (int tries = 0; !reader.ready() && tries < 1000; ++tries) reader.prime(source, budget);
    REQUIRE(reader.ready());
}

double amplitude(std::span<const float> samples, double frequency) {
    double real = 0, imaginary = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase = 2 * std::numbers::pi * frequency * double(i);
        real += double(samples[i]) * std::cos(phase);
        imaginary += double(samples[i]) * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / double(samples.size());
}
}  // namespace

TEST_CASE("playrate clock: fractional consumption and ramps ignore caller partitioning") {
    for (const double rate : {.25, .731, 1., 1.137, 4.}) {
        PlayrateClock whole, split;
        whole.reset(-123.5, rate);
        split.reset(-123.5, rate);
        whole.setRate(2.371, 480);
        split.setRate(2.371, 480);
        std::array<double, 4097> a{}, b{};
        clockRun(whole, a);
        std::size_t at = 0;
        for (std::size_t chunk : {1, 63, 137, 1024, 3, 2048, 820}) {
            clockRun(split, std::span(b).subspan(at, chunk + 1));
            at += chunk;
        }
        REQUIRE(at == 4096);
        CHECK(a == b);
        CHECK(whole.timeline() == split.timeline());
        CHECK(whole.rate() == 2.371);
        CHECK(a[1] - a[0] == doctest::Approx(rate));
    }
}

TEST_CASE("playrate clock: authored square steps are immediate and crossings are device offsets") {
    PlayrateClock clock;
    clock.reset(0, 1);
    clock.setEnvelope(square, nullptr);
    std::array<double, 465> points{};
    clockRun(clock, points);
    CHECK(points[400] == 100);
    CHECK(points[401] == 104);
    CHECK(PlayrateClock::crossing(points, 100) == 400);
    CHECK(PlayrateClock::crossing(points, 101) == 401);
    CHECK(PlayrateClock::crossing(points, -1) == 0);
    CHECK(PlayrateClock::crossing(points, 1e6) == points.size());
    clock.setEnvelope(nullptr, nullptr);
    clock.reset(9.25, 1);
    std::array<double, 1025> unity{};
    clockRun(clock, unity);
    for (std::size_t i = 0; i < unity.size(); ++i) CHECK(unity[i] == 9.25 + double(i));
}

TEST_CASE("tape reader: exact unity, fractional DC, stereo linking and anti-alias cutoff") {
    TapeTable tape;
    Source source;
    for (int i = -100; i < 100; ++i) {
        auto value = tape.read(source.reader(), i, 1);
        CHECK(value[0] == Source::read(&source, 0, i));
        CHECK(value[1] == -value[0]);
    }
    source.constant = true;
    for (double rate : {.25, 1.137, 2., 4.}) {
        const auto value = tape.read(source.reader(), 100.333, rate);
        CHECK(value[0] == doctest::Approx(.25).epsilon(1e-6));
        CHECK(value[1] == -value[0]);
    }
    source.constant = false;
    // At 4x, source .3 cycles/sample must not fold to device .2 cycles/sample.
    source.frequency = .3;
    double aliasPeak = 0;
    for (int i = 0; i < 2048; ++i)
        aliasPeak = std::max(aliasPeak, std::abs(double(tape.read(source.reader(), i * 4., 4)[0])));
    CHECK(aliasPeak < .0001);
    source.frequency = .01;
    std::array<float, 4096> pass{};
    for (std::size_t i = 0; i < pass.size(); ++i)
        pass[i] = tape.read(source.reader(), double(i) * 4., 4)[0];
    CHECK(amplitude(pass, .04) > .39);
}

TEST_CASE("streaming stretch: seek reseeds, priming budgets and caller groups are deterministic") {
    Source source;
    StretchReader a(48000), b(48000);
    constexpr std::size_t n = 64 * 512;
    std::vector<float> al(n), ar(n), bl(n), br(n);
    for (double rate : {.25, .731, 1., 1.137, 4.}) {
        CAPTURE(rate);
        start(a, source.reader(), rate, 1);
        start(b, source.reader(), rate, 1000);
        stretchRun(a, source.reader(), rate, al, ar);
        for (std::size_t at = 0; at < n; at += 256)
            stretchRun(b, source.reader(), rate, std::span(bl).subspan(at, 256),
                       std::span(br).subspan(at, 256));
        CHECK(al == bl);
        CHECK(ar == br);
        CHECK(std::all_of(al.begin(), al.end(), [](float v) { return std::isfinite(v); }));
        CHECK(amplitude(std::span(al).subspan(8192), source.frequency) > .25);
        if (rate == .25 || rate == 4.)
            CHECK(amplitude(std::span(al).subspan(8192), source.frequency * rate) < .04);
        start(a, source.reader(), rate, 3);
        stretchRun(a, source.reader(), rate, al, ar);
        CHECK(al == bl);
        CHECK(ar == br);
    }
}

TEST_CASE("streaming stretch: unprimed and discontinuous trajectories do not advance") {
    StretchReader reader(44100);
    Source source;
    reader.seek(source.reader(), 1000, .25);
    CHECK_FALSE(reader.ready());
    std::array<float, 64> left{}, right{};
    const auto pos = reader.inputPosition();
    CHECK_FALSE(reader.process(source.reader(), pos + 16, left, right));
    CHECK(reader.inputPosition() == pos);
    for (int tries = 0; !reader.ready() && tries < 100; ++tries) reader.prime(source.reader(), 1);
    REQUIRE(reader.ready());
    const auto primed = reader.inputPosition();
    for (double bad : {-64., 0., 15., 257., std::numeric_limits<double>::infinity()}) {
        CHECK_FALSE(reader.process(source.reader(), primed + bad, left, right));
        CHECK(reader.inputPosition() == primed);
    }
    CHECK(reader.process(source.reader(), primed + 16., left, right));
}
