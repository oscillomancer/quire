// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <random>
#include "gate/processor.h"
using namespace supple;
using namespace supple::gate;
namespace {
void dc(Processor& p, double amplitude, int samples, double keyValue = -1) {
    std::vector<double> l(samples, amplitude), r = l, k(samples, keyValue);
    const double* in[]{l.data(), r.data()};
    double* out[]{l.data(), r.data()};
    const double* key[]{k.data(), k.data()};
    p.process(in, out, static_cast<uint32_t>(samples), keyValue < 0 ? nullptr : key);
}
double db(double x) { return 20 * std::log10(std::max(1e-15, std::abs(x))); }
}  // namespace
TEST_CASE("gate metadata patches and paths") {
    REQUIRE(params::validateTable(table()));
    for (size_t i = 0; i < table().size(); ++i) CHECK(table()[i].id == i);
    CHECK((table()[kLookahead].flags & params::kAutomatable) == 0);
    for (bool alternate : {false, true}) {
        auto p = patchType().defaults();
        if (alternate)
            for (size_t i = 0; i < kGr; ++i) p.body.values[i] = table()[i].max;
        auto text = patchType().serialize(p);
        auto parsed = patchType().parse(text);
        REQUIRE(parsed);
        CHECK(patchType().serialize(*parsed) == text);
        CHECK(describe(p.body.values).size() <= 300);
        auto bad = json::parse(R"({"params":{"attack":-1,"sidechain":"wrong"}})");
        REQUIRE(bad);
        auto merged = patchType().merge(p, *bad);
        REQUIRE_FALSE(merged);
        REQUIRE_FALSE(merged.error().details.empty());
        CHECK(merged.error().details[0].path.find("/params/") == 0);
        CHECK(patchType().serialize(p) == text);
    }
}
TEST_CASE("gate one-pole times hold and hysteresis at five rates") {
    for (double fs : {44100., 48000., 88200., 96000., 192000.}) {
        Processor p;
        p.prepare(fs, 4096);
        p.setParam(kRange, -40);
        p.setParam(kAttack, 10);
        p.setParam(kHold, 20);
        p.setParam(kRelease, 50);
        dc(p, .1, static_cast<int>(std::round(fs * .01)));
        CHECK(p.telemetry().gr == doctest::Approx(-40 / std::numbers::e).epsilon(.001));
        CHECK(p.telemetry().open);
        dc(p, .1, static_cast<int>(fs * .2));
        dc(p, .0001, static_cast<int>(std::round(fs * .02)));
        CHECK(p.telemetry().open);
        CHECK(p.telemetry().gr == doctest::Approx(0).epsilon(.001));
        dc(p, .0001, static_cast<int>(std::round(fs * .05)) + 2);
        CHECK_FALSE(p.telemetry().open);
        CHECK(p.telemetry().gr == doctest::Approx(-40 * (1 - 1 / std::numbers::e)).epsilon(.002));
        Processor h;
        h.prepare(fs, 64);
        h.setParam(kHold, 0);
        dc(h, .02, 64);
        int changes = 0;
        bool before = h.telemetry().open;
        for (int i = 0; i < 1000; ++i) {
            dc(h, std::pow(10., (-40 + 2 * std::sin(i * .1)) / 20), 1);
            if (before != h.telemetry().open) ++changes;
            before = h.telemetry().open;
        }
        CHECK(changes == 0);
    }
}
TEST_CASE("gate finite floor and exact mute including missing external key") {
    for (double range : {-40., -80.}) {
        Processor p;
        p.prepare(48000, 256);
        p.setParam(kRange, range);
        p.setParam(kSidechain, 1);
        p.setParam(kRelease, 5);
        std::array<double, 256> l{}, r{};
        l.fill(.2);
        r.fill(.1);
        const double* in[]{l.data(), r.data()};
        double* out[]{l.data(), r.data()};
        p.process(in, out, 256);
        CHECK_FALSE(p.telemetry().open);
        if (range == -80) {
            CHECK(l[255] == 0);
            CHECK(r[255] == 0);
        } else {
            CHECK(db(l[255] / .2) == doctest::Approx(-40).epsilon(.001));
        }
        // Closing after a previously open burst must settle to the same floor.
        p.setParam(kSidechain, 0);
        dc(p, .1, 20000);
        p.setParam(kSidechain, 1);
        dc(p, .1, 20000);
        CHECK(p.telemetry().gr == doctest::Approx(range).epsilon(.001));
    }
}
TEST_CASE("expander static curve and stereo link") {
    for (double fs : {44100., 48000., 88200., 96000., 192000.})
        for (double level : {-70., -60., -50., -44., -40., -30., -10.}) {
            Processor p;
            p.prepare(fs, 4096);
            p.setParam(kMode, 1);
            p.setParam(kThreshold, -40);
            p.setParam(kRange, -40);
            p.setParam(kRatio, 2.5);
            p.setParam(kRelease, 5);
            dc(p, std::pow(10., level / 20), static_cast<int>(fs * .15));
            CHECK(std::abs(p.telemetry().gr - expansionDb(level, -40, 2.5, -40)) < .2);
        }
    for (double link : {0., 100.}) {
        Processor p;
        p.prepare(48000, 64);
        p.setParam(kStereoLink, link);
        p.setParam(kAttack, .01);
        std::vector<double> l(2048, .1), r(2048, .0001);
        const double* in[]{l.data(), r.data()};
        double* out[]{l.data(), r.data()};
        p.process(in, out, 2048);
        CHECK(l.back() == doctest::Approx(.1));
        if (link == 100)
            CHECK(r.back() == doctest::Approx(.0001));
        else
            CHECK(r.back() == 0);
    }
}
TEST_CASE("lookahead first transient latency dry alignment and active immutability") {
    for (double fs : {8000., 22050., 44100., 48000., 88200., 96000., 192000., 384000., 768000.})
        for (double choice : {0., 1., 2.}) {
            Processor p;
            p.prepare(fs, 4096);
            p.setParam(kLookahead, choice);
            p.setParam(kAttack, 100);
            const uint32_t delay = lookaheadSamples(choice, fs);
            CHECK(p.latency() == delay);
            std::vector<double> l(delay + 20), r(l.size());
            l[0] = r[0] = .5;
            const double* in[]{l.data(), r.data()};
            double* out[]{l.data(), r.data()};
            p.process(in, out, static_cast<uint32_t>(l.size()));
            if (delay) {
                CHECK(l[delay] == doctest::Approx(.5).epsilon(1e-10));
                for (uint32_t i = 0; i < delay; ++i) CHECK(l[i] == 0);
            }
            p.setParam(kLookahead, choice == 0 ? 2 : 0);
            CHECK(p.latency() == delay);
            Processor dry;
            dry.prepare(fs, 4096);
            dry.setParam(kLookahead, choice);
            dry.setParam(kMix, 0);
            std::fill(l.begin(), l.end(), 0);
            l[0] = .5;
            r = l;
            dry.process(in, out, static_cast<uint32_t>(l.size()));
            CHECK(l[delay] == .5);
        }
}
TEST_CASE("key filtering and listen are independent of main audio") {
    for (double hz : {50., 1000.}) {
        Processor p;
        p.prepare(48000, 64);
        p.setParam(kScHighpass, 200);
        p.setParam(kSidechain, 1);
        p.setParam(kScListen, 1);
        std::vector<double> main(48000, .1), key(48000), out(48000);
        for (size_t i = 0; i < key.size(); ++i)
            key[i] = .1 * std::sin(2 * std::numbers::pi * hz * i / 48000);
        const double *in[]{main.data(), main.data()}, *k[]{key.data(), key.data()};
        double* output[]{out.data(), out.data()};
        p.process(in, output, 48000, k);
        double energy = 0;
        for (size_t i = 24000; i < out.size(); ++i) energy += out[i] * out[i];
        const double measured = db(std::sqrt(energy / 24000) / (.1 / std::sqrt(2.)));
        if (hz == 50)
            CHECK(measured < -24);
        else
            CHECK(std::abs(measured) < .1);
    }
}
TEST_CASE("gate automation is block invariant and state deterministic") {
    constexpr int frames = 18000;
    std::vector<double> l(frames), r(frames), a(frames), b(frames);
    for (int i = 0; i < frames; ++i) {
        l[i] = .15 * std::sin(i * .13);
        r[i] = .08 * std::sin(i * .17);
    }
    std::vector<double> reference, referenceRight;
    for (int block : {1, 7, 64, 4096}) {
        Processor p;
        p.prepare(48000, 4096);
        p.setParam(kLookahead, 2);
        for (int pos = 0; pos < frames;) {
            if (pos % 131 == 0) {
                p.setParam(kThreshold, -60 + (pos / 131) % 60);
                p.setParam(kMode, (pos / 131) % 2);
                p.setParam(kScHighpass, 10 + (pos / 131) % 490);
                p.setParam(kMix, 40 + (pos / 131) % 61);
                p.setParam(kStereoLink, (pos / 131) % 101);
            }
            const int count = std::min({block, frames - pos, 131 - pos % 131});
            const double* in[]{l.data() + pos, r.data() + pos};
            double* out[]{a.data() + pos, b.data() + pos};
            p.process(in, out, count);
            pos += count;
        }
        if (reference.empty()) {
            reference = a;
            referenceRight = b;
        }
        CHECK(std::memcmp(reference.data(), a.data(), a.size() * sizeof(double)) == 0);
        CHECK(std::memcmp(referenceRight.data(), b.data(), b.size() * sizeof(double)) == 0);
    }
    auto patch = patchType().defaults();
    for (size_t i = 0; i < kGr; ++i)
        patch.body.values[i] = table()[i].min + (table()[i].max - table()[i].min) * .37;
    auto parsed = patchType().parse(patchType().serialize(patch));
    REQUIRE(parsed);
    Processor x, y;
    x.prepare(48000, 4096);
    y.prepare(48000, 4096);
    x.setParams(patch.body.values);
    y.setParams(parsed->body.values);
    const double* in[]{l.data(), r.data()};
    double* out[]{a.data(), b.data()};
    x.process(in, out, frames);
    auto saved = a;
    y.process(in, out, frames);
    CHECK(saved == a);
}
TEST_CASE("five seconds of dense automation stays finite and bounded") {
    Processor p;
    p.prepare(48000, 64);
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> noise(-.9, .9);
    std::array<double, 64> l{}, r{};
    for (int pos = 0; pos < 240000;) {
        const int count = std::min(1 + static_cast<int>(rng() % 64), 240000 - pos);
        for (size_t i = 0; i < kGr; ++i)
            p.setParam(i,
                       table()[i].min + (table()[i].max - table()[i].min) * (rng() % 1000) / 999.);
        for (int i = 0; i < count; ++i) l[i] = r[i] = noise(rng);
        double* io[]{l.data(), r.data()};
        p.process(io, count);
        for (int i = 0; i < count; ++i) {
            REQUIRE(std::isfinite(l[i]));
            REQUIRE(std::abs(l[i]) < 100);
        }
        pos += count;
    }
}

TEST_CASE("lookahead opening is monotonic without a terminal gain jump") {
    Processor p;
    p.prepare(48000, 256);
    p.setParam(kSidechain, 1);
    p.setParam(kLookahead, 1);
    p.setParam(kAttack, 100);
    dc(p, .02, 1000, 0);
    std::vector<double> main(80, .02), key(80, .1), left(80), right(80);
    const double *in[]{main.data(), main.data()}, *k[]{key.data(), key.data()};
    double* out[]{left.data(), right.data()};
    p.process(in, out, 80, k);
    for (int i = 1; i < 48; ++i) {
        CHECK(left[i] >= left[i - 1]);
        CHECK(left[i] - left[i - 1] < .004);
    }
    CHECK(left[48] == doctest::Approx(.02).epsilon(1e-10));
}
TEST_CASE("mono precision null after opening and nonfinite inputs recover") {
    Processor p;
    p.prepare(48000, 64, 1);
    dc(p, .1, 10000);
    std::array<float, 64> a{}, copy{};
    for (size_t i = 0; i < a.size(); ++i) a[i] = copy[i] = .1f;
    float* io[]{a.data()};
    p.process(io, 64);
    CHECK(std::memcmp(a.data(), copy.data(), sizeof(a)) == 0);
    a[0] = std::numeric_limits<float>::quiet_NaN();
    a[1] = std::numeric_limits<float>::infinity();
    a[2] = std::numeric_limits<float>::denorm_min();
    p.process(io, 64);
    for (float v : a) CHECK(std::isfinite(v));
    p.reset();
    a.fill(0);
    p.process(io, 64);
    for (float v : a) CHECK(v == 0);
}
