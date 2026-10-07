// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <vector>
#include "utility/processor.h"
using namespace supple;
using namespace supple::utility;
namespace {
struct Audio {
    std::vector<double> l, r;
    explicit Audio(size_t n) : l(n), r(n) {}
    void process(Processor& p, size_t start = 0, size_t count = 0) {
        double* io[] = {l.data() + start, r.data() + start};
        p.process(io, static_cast<int>(count ? count : l.size() - start));
    }
};
Audio signal(size_t n) {
    Audio a(n);
    for (size_t i = 0; i < n; ++i) {
        a.l[i] = std::sin(static_cast<double>(i) * .113) * .5;
        a.r[i] = std::cos(static_cast<double>(i) * .079) * .25;
    }
    return a;
}
}  // namespace
TEST_CASE("utility table, canonical patches, merge paths and descriptions") {
    REQUIRE(params::validateTable(kParams));
    for (size_t i = 0; i < kNumParams; ++i) CHECK(kParams[i].id == i);
    auto p = patchType().defaults();
    auto saved = patchType().serialize(p);
    auto loaded = patchType().parse(saved);
    REQUIRE(loaded);
    CHECK(loaded->body == p.body);
    CHECK(patchType().serialize(*loaded) == saved);
    for (size_t i = 0; i < 10; ++i) p.body.values[i] = kParams[i].max;
    saved = patchType().serialize(p);
    loaded = patchType().parse(saved);
    REQUIRE(loaded);
    CHECK(loaded->body == p.body);
    auto bad = json::parse(R"({"params":{"width":201}})");
    REQUIRE(bad);
    auto merged = patchType().merge(p, *bad);
    CHECK_FALSE(merged);
    CHECK(merged.error().path == "/params/width");
    Values v = defaults();
    CHECK(describe(v) == "Unity stereo");
    for (size_t i = 0; i < 10; ++i) v[i] = kParams[i].max;
    CHECK(describe(v).size() <= 300);
    CHECK(describe(v).find("Gain") != std::string::npos);
}
TEST_CASE("utility default null is bit exact for float/double, mono/stereo and rates") {
    for (double sr : {22050., 44100., 48000., 88200., 96000., 192000., 384000.}) {
        for (int channels : {1, 2}) {
            Processor p;
            p.prepare(sr, 4096, channels);
            auto a = signal(4096);
            a.l[0] = -0.0;
            a.l[1] = std::numeric_limits<double>::denorm_min();
            auto b = a;
            a.process(p);
            CHECK(std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(double)) == 0);
            if (channels == 2) CHECK(a.r == b.r);
            p.reset();
            float l[] = {-0.0f, .125f, -.5f, std::numeric_limits<float>::denorm_min()},
                  r[] = {0.f, .25f, -.125f, .5f};
            float expected[4];
            std::memcpy(expected, l, sizeof(l));
            float* io[] = {l, r};
            p.process(io, 4);
            CHECK(std::memcmp(l, expected, sizeof(l)) == 0);
            CHECK(l[1] == .125f);
            CHECK(l[2] == -.5f);
            CHECK(std::signbit(l[0]));
        }
    }
}
TEST_CASE("utility gain, balance, width, channels, polarity, swap and mute") {
    auto measure = [](size_t param, double value) {
        Processor p;
        p.prepare(48000, 1);
        p.setParam(param, value);
        Audio a(1);
        a.l[0] = .4;
        a.r[0] = .2;
        a.process(p);
        return a;
    };
    CHECK(measure(kGain, -120).l[0] == 0);
    CHECK(measure(kGain, 6).l[0] == doctest::Approx(.4 * std::pow(10., .3)).epsilon(1e-12));
    CHECK(measure(kGain, 20 * std::log10(2.)).l[0] == doctest::Approx(.8));
    CHECK(measure(kWidth, 0).l[0] == doctest::Approx(.3));
    CHECK(measure(kWidth, 0).r[0] == doctest::Approx(.3));
    CHECK(measure(kWidth, 200).l[0] == doctest::Approx(.5));
    CHECK(measure(kWidth, 200).r[0] == doctest::Approx(.1));
    CHECK(measure(kPan, 100).l[0] == 0);
    CHECK(measure(kPan, 100).r[0] == .2);
    CHECK(measure(kPan, 50).l[0] == .2);
    CHECK(measure(kPan, -50).r[0] == .1);
    CHECK(measure(kChannels, 1).r[0] == .4);
    CHECK(measure(kChannels, 2).l[0] == .2);
    CHECK(measure(kChannels, 3).l[0] == doctest::Approx(.3));
    CHECK(measure(kChannels, 4).r[0] == doctest::Approx(.1));
    CHECK(measure(kSwap, 1).l[0] == .2);
    CHECK(measure(kInvertLeft, 1).l[0] == -.4);
    CHECK(measure(kInvertRight, 1).r[0] == -.2);
    CHECK(measure(kMute, 1).l[0] == 0);
}
TEST_CASE("utility bass mono matches LR4, preserves M, sample-rate sweep") {
    for (double sr : {44100., 48000., 88200., 96000., 192000.})
        for (double hz : {50., 1000.}) {
            Processor p;
            p.prepare(sr, 4096);
            p.setParam(kMonoBelow, 150);
            Audio a(static_cast<size_t>(sr));
            for (size_t i = 0; i < a.l.size(); ++i) {
                const double s =
                    .3 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / sr);
                const double m = .2 * std::sin(.003 * static_cast<double>(i));
                a.l[i] = m + s;
                a.r[i] = m - s;
            }
            auto in = a;
            a.process(p);
            double input = 0, output = 0;
            for (size_t i = a.l.size() / 2; i < a.l.size(); ++i) {
                CHECK((a.l[i] + a.r[i]) * .5 ==
                      doctest::Approx((in.l[i] + in.r[i]) * .5).epsilon(1e-12));
                input += std::pow((in.l[i] - in.r[i]) * .5, 2);
                output += std::pow((a.l[i] - a.r[i]) * .5, 2);
            }
            double db = 10 * std::log10(output / input),
                   ratio =
                       std::tan(std::numbers::pi * 150 / sr) / std::tan(std::numbers::pi * hz / sr);
            double expected = -20 * std::log10(1 + std::pow(ratio, 4));
            CHECK(std::abs(db - expected) < .02);
            if (hz == 50)
                CHECK(db < -38);
            else
                CHECK(std::abs(db) < .1);
            CHECK(p.latency() == 0);
        }
}
TEST_CASE("utility DC rejection, 10 ms mute ramp and telemetry") {
    Processor p;
    p.prepare(48000, 48000);
    p.setParam(kDcBlock, 1);
    Audio a(48000);
    std::fill(a.l.begin(), a.l.end(), .5);
    std::fill(a.r.begin(), a.r.end(), .5);
    a.process(p);
    CHECK(std::abs(a.l.back()) < 1e-12);
    p.setParam(kDcBlock, 0);
    p.reset();
    double l = 1, r = 1;
    double* io[] = {&l, &r};
    p.process(io, 1, true);
    auto t = p.takeTelemetry();
    CHECK(t.correlation == 1);
    CHECK(t.outputPeakDb == 0);
    p.setParam(kMute, 1);
    Audio ramp(480);
    std::fill(ramp.l.begin(), ramp.l.end(), 1);
    std::fill(ramp.r.begin(), ramp.r.end(), 1);
    ramp.process(p);
    CHECK(ramp.l[0] > 0.99);
    CHECK(ramp.l[239] == doctest::Approx(.5));
    CHECK(ramp.l[479] == 0);
    Processor mono;
    mono.prepare(48000, 1, 1);
    mono.setParam(kPan, 100);
    double sample = .5;
    double* monoIo[] = {&sample};
    mono.process(monoIo, 1, true);
    CHECK(sample == 0);
    auto monoMeter = mono.takeTelemetry();
    CHECK(monoMeter.outputPeakDb == -120);
    CHECK(monoMeter.correlation == 0);
}
TEST_CASE("utility block partitions and restored state give identical output under automation") {
    auto render = [](int block) {
        Processor p;
        p.prepare(48000, 4096);
        auto a = signal(16000);
        size_t pos = 0;
        while (pos < a.l.size()) {
            if (pos % 173 == 0) {
                p.setParam(kWidth, static_cast<double>((pos / 173) % 3) * 100);
                p.setParam(kMonoBelow, pos % 346 ? 150 : 0);
                p.setParam(kPan, pos % 346 ? 50 : -50);
                p.setParam(kChannels, static_cast<double>((pos / 173) % 5));
                p.setParam(kMute, (pos / 173) % 7 == 0 ? 1 : 0);
            }
            size_t next = (pos / 173 + 1) * 173;
            size_t n = std::min({static_cast<size_t>(block), next - pos, a.l.size() - pos});
            a.process(p, pos, n);
            pos += n;
        }
        return a;
    };
    auto one = render(1);
    for (int block : {7, 64, 4096}) {
        auto a = render(block);
        CHECK(a.l == one.l);
        CHECK(a.r == one.r);
    }
    std::mt19937 rng(9);
    auto patch = patchType().defaults();
    for (size_t i = 0; i < 10; ++i) {
        double v = kParams[i].min +
                   (kParams[i].max - kParams[i].min) * (static_cast<double>(rng()) / rng.max());
        patch.body.values[i] = params::clampPhysical(kParams[i], v);
    }
    auto saved = patchType().serialize(patch);
    auto loaded = patchType().parse(saved);
    REQUIRE(loaded);
    Processor p, q;
    p.prepare(48000, 4096);
    q.prepare(48000, 4096);
    for (size_t i = 0; i < 10; ++i) {
        p.setParam(i, patch.body.values[i]);
        q.setParam(i, loaded->body.values[i]);
    }
    auto a = signal(4096), b = a;
    a.process(p);
    b.process(q);
    CHECK(a.l == b.l);
    CHECK(a.r == b.r);
}
TEST_CASE("utility five seconds full-range automation is finite and bounded") {
    std::mt19937 rng(123);
    Processor p;
    p.prepare(48000, 64);
    Audio a(64);
    for (int pos = 0; pos < 240000;) {
        int n = 1 + static_cast<int>(rng() % 64);
        for (size_t i = 0; i < 10; ++i) {
            const auto& spec = kParams[i];
            double v = spec.min + (spec.max - spec.min) * (static_cast<double>(rng()) / rng.max());
            p.setParam(i, v);
        }
        for (int i = 0; i < n; ++i) {
            a.l[static_cast<size_t>(i)] = static_cast<double>(rng()) / rng.max() - .5;
            a.r[static_cast<size_t>(i)] = static_cast<double>(rng()) / rng.max() - .5;
        }
        a.process(p, 0, static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            CHECK(std::isfinite(a.l[static_cast<size_t>(i)]));
            CHECK(std::isfinite(a.r[static_cast<size_t>(i)]));
            CHECK(std::abs(a.l[static_cast<size_t>(i)]) < 100);
            CHECK(std::abs(a.r[static_cast<size_t>(i)]) < 100);
        }
        pos += n;
    }
}

TEST_CASE("utility unity keeps non-finite bits without poisoning stateful paths") {
    for (double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()}) {
        for (auto stateful : {kDcBlock, kMonoBelow}) {
            for (bool initiallyOn : {false, true}) {
                Processor clean, poisoned;
                clean.prepare(48000, 1024);
                poisoned.prepare(48000, 1024);
                const double setting = stateful == kDcBlock ? 1 : 150;
                if (initiallyOn) {
                    clean.setParam(stateful, setting);
                    poisoned.setParam(stateful, setting);
                }
                Audio a(1024), b(1024);
                b.l[0] = bad;
                a.process(clean);
                b.process(poisoned);
                if (!initiallyOn)
                    CHECK(bool(std::memcmp(&b.l[0], &bad, sizeof(bad)) == 0));
                else {
                    CHECK(bool(a.l == b.l));
                    CHECK(bool(a.r == b.r));
                }
                clean.setParam(stateful, setting);
                poisoned.setParam(stateful, setting);
                a = signal(1024);
                b = a;
                a.process(clean);
                b.process(poisoned);
                CHECK(bool(a.l == b.l));
                CHECK(bool(a.r == b.r));
            }
        }
    }
}
