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
#include "playrate_timeline.h"
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
    static const float* fetch(const void* context, std::size_t channel, std::int64_t first,
                              std::span<float> scratch) noexcept {
        for (std::size_t i = 0; i < scratch.size(); ++i)
            scratch[i] =
                read(context, static_cast<int>(channel), first + static_cast<std::int64_t>(i));
        return scratch.data();
    }
    RateSource reader() const { return {this, fetch, 2}; }
};

double square(const void*, double sample) noexcept { return sample < 100 ? .25 : 4; }

const float* impulse(const void*, std::size_t, std::int64_t first,
                     std::span<float> scratch) noexcept {
    for (std::size_t i = 0; i < scratch.size(); ++i)
        scratch[i] = first + static_cast<std::int64_t>(i) == 12000 ? 1.f : 0.f;
    return scratch.data();
}

void clockRun(PlayrateClock& clock, std::span<double> output) SUPPLE_RT { clock.advance(output); }

void stretchRun(StretchReader& reader, RateSource source, double rate, std::span<float> left,
                std::span<float> right) SUPPLE_RT {
    supple::dsp::DenormalGuard denormals;
    while (!left.empty()) {
        float* outputs[] = {left.data(), right.data()};
        const bool ok =
            reader.process(source, reader.inputPosition() + rate * kRateQuantum, outputs);
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
    TapeReader tape(std::make_shared<TapeTable>(), 2);
    auto tapeAt = [&](RateSource source, double pos, double rate) {
        std::array<double, 65> positions{};
        for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = pos + double(i) * rate;
        std::array<float, 64> left{}, right{};
        float* outputs[] = {left.data(), right.data()};
        REQUIRE(tape.process(source, positions, outputs));
        return std::array<float, 2>{left[0], right[0]};
    };
    Source source;
    for (int i = -100; i < 100; ++i) {
        auto value = tapeAt(source.reader(), i, 1);
        CHECK(value[0] == Source::read(&source, 0, i));
        CHECK(value[1] == -value[0]);
    }
    source.constant = true;
    for (double rate : {.25, 1.137, 2., 4.}) {
        const auto value = tapeAt(source.reader(), 100.333, rate);
        CHECK(value[0] == doctest::Approx(.25).epsilon(1e-6));
        CHECK(value[1] == -value[0]);
    }
    source.constant = false;
    // At 4x, source .3 cycles/sample must not fold to device .2 cycles/sample.
    source.frequency = .3;
    double aliasPeak = 0;
    for (int i = 0; i < 2048; ++i)
        aliasPeak = std::max(aliasPeak, std::abs(double(tapeAt(source.reader(), i * 4., 4)[0])));
    CHECK(aliasPeak < .0001);
    source.frequency = .01;
    std::array<float, 4096> pass{};
    for (std::size_t i = 0; i < pass.size(); ++i)
        pass[i] = tapeAt(source.reader(), double(i) * 4., 4)[0];
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
    float* outputs[] = {left.data(), right.data()};
    const auto pos = reader.inputPosition();
    CHECK_FALSE(reader.process(source.reader(), pos + 16, outputs));
    CHECK(reader.inputPosition() == pos);
    for (int tries = 0; !reader.ready() && tries < 100; ++tries) reader.prime(source.reader(), 1);
    REQUIRE(reader.ready());
    const auto primed = reader.inputPosition();
    for (double bad : {-64., 0., 15., 257., std::numeric_limits<double>::infinity()}) {
        CHECK_FALSE(reader.process(source.reader(), primed + bad, outputs));
        CHECK(reader.inputPosition() == primed);
    }
    CHECK(reader.process(source.reader(), primed + 16., outputs));
}

TEST_CASE("streaming stretch: primed output aligns with the requested source time") {
    StretchReader reader(48000);
    const RateSource source{nullptr, impulse, 2};
    for (double rate : {.25, 1., 4.}) {
        CAPTURE(rate);
        start(reader, source, rate, 1);
        std::vector<float> left(64 * 1024), right(left.size());
        stretchRun(reader, source, rate, left, right);
        const auto peak = std::max_element(
            left.begin(), left.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
        const double at = double(peak - left.begin());
        // Stretching can move a transient within a synthesis hop. At unity,
        // however, the impulse must align exactly: missing analysis lookahead
        // shifted it by 2880 frames (and by 11520 device frames at 0.25x).
        if (rate == 1.)
            CHECK(at == 12000);
        else
            CHECK(std::abs(at - 12000. / rate) < 1440);
    }
}

TEST_CASE("source conversion: mono and multichannel use one block fetch per channel") {
    struct Memory {
        std::vector<std::vector<float>> channels;
        mutable std::size_t calls = 0;
        static const float* fetch(const void* ctx, std::size_t channel, std::int64_t first,
                                  std::span<float> scratch) noexcept {
            const auto& self = *static_cast<const Memory*>(ctx);
            ++self.calls;
            const auto& data = self.channels[channel];
            if (first >= 0 && static_cast<std::size_t>(first) + scratch.size() <= data.size())
                return data.data() + first;
            for (std::size_t i = 0; i < scratch.size(); ++i) {
                const auto at = first + static_cast<std::int64_t>(i);
                scratch[i] = at >= 0 && static_cast<std::size_t>(at) < data.size()
                                 ? data[static_cast<std::size_t>(at)]
                                 : 0.f;
            }
            return scratch.data();
        }
    };
    auto table = std::make_shared<TapeTable>();
    for (std::size_t channels : {1u, 2u, 6u}) {
        CAPTURE(channels);
        Memory memory;
        memory.channels.assign(channels, std::vector<float>(20000));
        for (std::size_t c = 0; c < channels; ++c)
            for (std::size_t i = 0; i < memory.channels[c].size(); ++i)
                memory.channels[c][i] =
                    static_cast<float>((c + 1) * .01 * std::sin(double(i) * .04));
        const RateSource source{&memory, Memory::fetch, channels};
        TapeReader tape(table, channels);
        StretchReader stretch(48000, channels);
        std::vector<std::array<float, 64>> data(channels);
        std::vector<float*> outputs(channels);
        for (std::size_t c = 0; c < channels; ++c) outputs[c] = data[c].data();
        std::array<double, 65> positions{};
        for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = 1000 + 1.137 * double(i);
        REQUIRE(tape.process(source, positions, outputs));
        CHECK(memory.calls == channels);
        for (std::size_t c = 1; c < channels; ++c)
            for (std::size_t i = 0; i < 64; ++i)
                CHECK(data[c][i] ==
                      doctest::Approx(double(data[0][i]) * double(c + 1)).epsilon(1e-5));
        start(stretch, source, 1.137, 1);
        memory.calls = 0;
        REQUIRE(stretch.process(source, stretch.inputPosition() + 1.137 * 64, outputs));
        CHECK(memory.calls == channels);
        for (const auto& channel : data)
            for (float sample : channel) CHECK(std::isfinite(sample));
    }
}

TEST_CASE("timeline window: partitioning, PDC history, loops and plan adoption share phase") {
    using supple::model::PointShape;
    const std::vector<RatePoint> points{{0, .731, PointShape::Linear, 0},
                                        {10000, 2.137, PointShape::Square, 0},
                                        {12000, .25, PointShape::Linear, 0}};
    PlayrateTimeline a(16384, 1, points), b(16384, 1, points);
    a.reset(123, 100.5);
    b.reset(123, 100.5);
    a.loop(1000, 12000, true);
    b.loop(1000, 12000, true);
    a.ensure(14000);
    for (std::int64_t at = 123; at < 14000; at += 127) b.ensure(std::min<std::int64_t>(at, 14000));
    b.ensure(14000);
    for (std::int64_t at = 0; at <= 14000; ++at) {
        CHECK(a.phase(at) == b.phase(at));
        CHECK(a.position(at) == b.position(at));
        CHECK(a.rate(at) == b.rate(at));
    }
    // Adoption is anchored at consumption/render boundary, never at the last
    // lookahead sample. PDC reads the preceding device frames' old trajectory.
    PlayrateTimeline next(16384, 1, points);
    next.adopt(a, 9001, 480);
    next.ensure(14000);
    for (std::int64_t at = 8000; at <= 14000; ++at) {
        CHECK(next.phase(at) == a.phase(at));
        CHECK(next.position(at) == a.position(at));
        CHECK(next.rate(at) == a.rate(at));
    }
    PlayrateTimeline scalar(16384, 1, {}), changed(16384, 2, {});
    scalar.ensure(1000);
    changed.adopt(scalar, 333, 480);
    changed.ensure(1000);
    CHECK(changed.phase(333) == 333);
    CHECK(changed.phase(334) == 334);
    CHECK(changed.rate(813) == 2);
    CHECK(changed.phase(332) == scalar.phase(332));
}

TEST_CASE("playrate timeline: manual transitions wait for compensated lookahead") {
    PlayrateTimeline previous(262144, 1.137, {}), next(262144, 2, {});
    previous.ensure(40000);
    next.adopt(previous, 1024, 480, 32768);
    next.ensure(40000);
    for (std::int64_t device = 1024; device <= 33792; ++device) {
        CHECK(next.phase(device) == previous.phase(device));
        CHECK(next.rate(device) == 1.137);
    }
    CHECK(next.rate(33793) > 1.137);
    CHECK(next.rate(34272) == 2);
}

TEST_CASE("playrate timeline: nonzero range origins share absolute clock accumulation") {
    std::vector<RatePoint> absolute{{0, .5, supple::model::PointShape::Linear, 0},
                                    {960, 2, supple::model::PointShape::Linear, 0}};
    auto relative = absolute;
    for (auto& point : relative) point.sample -= 336;
    PlayrateTimeline seek(32768, 1.137, absolute), range(32768, 1.137, relative, 336);
    seek.reset(336, 336);
    range.reset(0, 0);
    seek.ensure(3336);
    range.ensure(3000);
    for (std::int64_t device = 0; device < 3000; ++device) {
        CHECK(seek.absolutePhase(device + 336) == range.absolutePhase(device));
        CHECK(seek.rate(device + 336) == range.rate(device));
    }
}

TEST_CASE("playrate timeline: retained history is tagged across ring wrap") {
    PlayrateTimeline clock(256, .8, {});
    CHECK(clock.hasHistory(-128, 0));
    CHECK_FALSE(clock.hasHistory(-129, 0));
    clock.ensure(320);
    CHECK_FALSE(clock.hasHistory(0, 320));
    CHECK(clock.hasHistory(65, 320));
    CHECK_FALSE(clock.hasHistory(65, 321));
}

TEST_CASE("playrate timeline: small graph edits retain earlier variable-rate history") {
    PlayrateTimeline original(2048, 1,
                              {{0, .5, supple::model::PointShape::Linear, 0},
                               {1000, 2, supple::model::PointShape::Linear, 0}});
    original.ensure(1400);
    PlayrateTimeline copied(2048, 1, original.points());
    copied.seed(original.snapshot(1000));
    REQUIRE(copied.copyPast(original, 936, 1000));  // edit currently needs just 64 frames
    PlayrateTimeline adopted(2048, 1, original.points());
    copied.ensure(1400);
    adopted.adopt(copied, 1100, 0, 0, false);
    adopted.ensure(1400);
    REQUIRE(adopted.hasHistory(200, 1100));
    for (std::int64_t at = 200; at <= 1100; ++at) {
        CHECK(adopted.absolutePhase(at) == original.absolutePhase(at));
        CHECK(adopted.rate(at) == original.rate(at));
    }
    // An oversized new window must not make up history beyond the old floor.
    PlayrateTimeline oversized(8192, 1, original.points());
    oversized.seed(original.snapshot(1000));
    CHECK_FALSE(oversized.copyPast(original, -100, 1000));
    CHECK_FALSE(oversized.hasHistory(-100, 1000));
}
