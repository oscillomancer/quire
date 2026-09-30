// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include "delay/processor.h"
#include "supple/base/rng.h"
#include "supple/json/json.h"
using namespace supple;
using namespace supple::delay;
namespace {
constexpr double pi = std::numbers::pi;
Values wet(double time = 40) {
    auto v = defaults();
    v[kSync] = 0;
    v[kTime] = time;
    v[kMix] = 100;
    v[kHighpass] = 20;
    v[kLowpass] = 20000;
    return v;
}
struct Audio {
    std::vector<double> l, r;
};
Audio render(Values v, double fs, int frames, int block = 256, bool events = false) {
    Processor p;
    REQUIRE(p.prepare(fs, 4096));
    p.setParams(v);
    Audio a{std::vector<double>(size_t(frames)), std::vector<double>(size_t(frames))};
    std::vector<double> l(size_t(frames), 0), r(size_t(frames), 0);
    for (int i = 0; i < frames; ++i) {
        l[size_t(i)] = .05 * std::sin(2 * pi * 733 * i / fs);
        r[size_t(i)] = .03 * std::sin(2 * pi * 1000 * i / fs);
    }
    for (int off = 0; off < frames;) {
        if (events && off % 997 == 0) {
            p.setParam(kTime, 10 + off % 500);
            p.setParam(kMode, (off / 997) % 3);
            p.setParam(kOffset, off % 101 - 50);
            p.setParam(kModDepth, 30);
            p.setParam(kFreeze, (off / 997) % 7 == 3);
            p.setTempo(80 + off % 80);
        }
        int n = std::min(block, frames - off);
        if (events) n = std::min(n, 997 - off % 997);
        const double* in[]{l.data() + off, r.data() + off};
        double* out[]{a.l.data() + off, a.r.data() + off};
        p.process(in, out, uint32_t(n));
        off += n;
    }
    return a;
}
Audio impulse(Values v, double fs, int frames) {
    Processor p;
    REQUIRE(p.prepare(fs, 4096));
    p.setParams(v);
    Audio a{std::vector<double>(size_t(frames)), std::vector<double>(size_t(frames))};
    std::vector<double> x(size_t(frames), 0), silent(size_t(frames), 0);
    x[0] = .1;
    const double* in[]{x.data(), silent.data()};
    double* out[]{a.l.data(), a.r.data()};
    p.process(in, out, uint32_t(frames));
    return a;
}
double energy(const std::vector<double>& x, size_t start = 0) {
    double e = 0;
    for (size_t i = start; i < x.size(); ++i) e += x[i] * x[i];
    return e;
}
}  // namespace
TEST_CASE("Delay metadata and canonical default/full patches") {
    REQUIRE(params::validateTable(table()));
    const auto& type = patchType();
    auto d = type.defaults(), full = d;
    for (size_t i = 0; i < kNumParams; ++i) {
        CHECK(table()[i].id == i);
        full.body.values[i] = table()[i].max;
    }
    for (const auto* p : {&d, &full}) {
        auto text = type.serialize(*p);
        auto parsed = type.parse(text);
        REQUIRE(parsed);
        CHECK(parsed->body == p->body);
        CHECK(type.serialize(*parsed) == text);
    }
    auto bad = json::parse(R"({"params":{"time":0,"mode":"bogus"}})");
    REQUIRE(bad);
    auto m = type.merge(d, *bad);
    REQUIRE_FALSE(m);
    CHECK_FALSE((m.error().path.empty() && m.error().details.empty()));
    CHECK(describe(full.body.values).size() <= 300);
    CHECK(lineTimes(defaults())[0] == 375);
    auto v = wet(5000);
    v[kOffset] = 50;
    CHECK(lineTimes(v)[1] == 5000);
    v[kTime] = 1;
    v[kOffset] = -50;
    CHECK(lineTimes(v)[1] == 1);
    v[kSync] = 1;
    CHECK(lineTimes(v, 1)[0] == 5000);
    CHECK(lineTimes(defaults(), 0)[0] == 375);
}
TEST_CASE("Delay fractional impulse timing and repeat response at every rate") {
    for (double fs : {22050., 44100., 48000., 88200., 96000., 192000., 384000.}) {
        auto v = wet(17.37);
        v[kFeedback] = 0;
        const int delay = int(std::round(v[kTime] * fs / 1000));
        auto a = impulse(v, fs, delay + 100);
        auto referenceValues = v;
        const int referenceDelay = int(std::ceil(fs * .001));
        referenceValues[kTime] = 1000 * referenceDelay / fs;
        auto reference = impulse(referenceValues, fs, referenceDelay + 100);
        int bestLag = 0;
        double best = -1;
        for (int delta = -2; delta <= 2; ++delta) {
            const int lag = delay - referenceDelay + delta;
            double cross = 0;
            for (int i = referenceDelay; i < referenceDelay + 97; ++i)
                cross += reference.l[size_t(i)] * a.l[size_t(i + lag)];
            if (cross > best) {
                best = cross;
                bestLag = lag;
            }
        }
        CHECK(std::abs(bestLag + referenceDelay - delay) <= 1);
        CHECK(energy(a.r) == 0);
        CHECK(energy(a.l) > 0);
    }
    auto v = wet(40);
    v[kFeedback] = 50;
    auto a = impulse(v, 48000, 7000);
    // Measure the passband component: even the widest filters color a broadband impulse.
    double amplitude[3]{};
    for (int k = 0; k < 3; ++k) {
        double real = 0, imaginary = 0;
        for (int i = 0; i < 1000; ++i) {
            const double sample = a.l[size_t((k + 1) * 1920 + i)];
            real += sample * std::cos(2 * pi * 1000 * i / 48000);
            imaginary += sample * std::sin(2 * pi * 1000 * i / 48000);
        }
        amplitude[k] = std::hypot(real, imaginary);
    }
    for (int k = 1; k < 3; ++k)
        CHECK(std::abs(20 * std::log10(amplitude[k] / amplitude[k - 1]) - 20 * std::log10(.5)) <
              .2);
}
TEST_CASE("Delay dry path null, deterministic modulation and exact partitions with automation") {
    auto v = defaults();
    v[kMix] = 0;
    const auto a = render(v, 48000, 3000);
    for (size_t i = 0; i < a.l.size(); ++i)
        CHECK(a.l[i] == .05 * std::sin(2 * pi * 733 * double(i) / 48000));
    v = wet();
    v[kModDepth] = 60;
    const auto reference = render(v, 48000, 30000, 1, true);
    for (int block : {7, 64, 4096}) {
        auto other = render(v, 48000, 30000, block, true);
        CHECK(reference.l == other.l);
        CHECK(reference.r == other.r);
    }
    auto patch = patchType().defaults();
    patch.body.values.assign(v.begin(), v.end());
    auto restored = patchType().parse(patchType().serialize(patch));
    REQUIRE(restored);
    Values values{};
    std::copy(restored->body.values.begin(), restored->body.values.end(), values.begin());
    CHECK(render(values, 48000, 30000).l == render(v, 48000, 30000).l);
}
TEST_CASE("Delay drive ramp from bypass to 2 dB has no loud echo level step") {
    const auto check = []<class T>() {
        Processor bypass, driven;
        REQUIRE(bypass.prepare(48000, 1));
        REQUIRE(driven.prepare(48000, 1));
        auto v = wet(10);
        v[kFeedback] = 0;
        bypass.setParams(v);
        driven.setParams(v);
        // Start the existing 20 ms parameter ramp on a settled 0.5-amplitude echo peak.
        constexpr int start = 9600 + 12, period = 48;
        double previousDb = 0, largestStep = 0, finalDb = 0;
        for (int n = 0; n <= start + 1200; ++n) {
            if (n == start) driven.setParam(kDrive, 2);
            const T sample = T(.5 * std::sin(2 * pi * n / period));
            const T* in[]{&sample, &sample};
            T reference[2]{}, output[2]{};
            T* referenceOut[]{&reference[0], &reference[1]};
            T* drivenOut[]{&output[0], &output[1]};
            bypass.process(in, referenceOut, 1);
            driven.process(in, drivenOut, 1);
            if (n < start || (n - start) % period != 0) continue;
            REQUIRE(std::abs(double(reference[0])) > .45);
            finalDb = 20 * std::log10(std::abs(double(output[0]) / double(reference[0])));
            if (n == start) CHECK(std::abs(finalDb) < .01);
            largestStep = std::max(largestStep, std::abs(finalDb - previousDb));
            previousDb = finalDb;
        }
        // Compare equal-phase peaks: ordinary waveform slopes are not level steps.
        CHECK(largestStep < .15);
        CHECK(finalDb < -.8);
        CHECK(finalDb > -2);
    };
    SUBCASE("32-bit audio") { check.operator()<float>(); }
    SUBCASE("64-bit audio") { check.operator()<double>(); }
}
TEST_CASE("Delay ping-pong offset, mono and width") {
    auto v = wet(40);
    v[kMode] = 1;
    v[kFeedback] = 50;
    v[kOffset] = 50;
    const auto a = impulse(v, 48000, 13000);
    CHECK(std::abs(a.l[1920]) > .01);
    CHECK(std::abs(a.r[1920]) < 1e-10);
    CHECK(std::abs(a.r[4800]) > .005);
    CHECK(std::abs(a.l[4800]) < 1e-5);
    CHECK(std::abs(a.l[6720]) > .002);
    v[kMode] = 2;
    v[kWidth] = 200;
    auto b = render(v, 48000, 10000);
    CHECK(b.l == b.r);
    v[kMode] = 0;
    v[kWidth] = 0;
    b = render(v, 48000, 10000);
    CHECK(b.l == b.r);
    Processor mono;
    REQUIRE(mono.prepare(48000, 128, 1));
    mono.setParams(v);
    double x[128]{};
    const double* in[]{x};
    double* out[]{x};
    mono.process(in, out, 128);
    CHECK(mono.latency() == 0);
}
TEST_CASE("Delay tempo and time glide are continuous with no read-head reset") {
    Processor p;
    REQUIRE(p.prepare(48000, 256));
    double x[256]{}, y[256]{}, r[256]{};
    const double* in[]{x, x};
    double* out[]{y, r};
    auto v = wet(375);
    p.setParams(v);
    double last = 0, previous = 0, maxSecond = 0;
    for (int off = 0; off < 96000; off += 256) {
        if (off == 48128) {
            p.setParam(kTime, 200);
        }
        for (int i = 0; i < 256; ++i) x[i] = .1 * std::sin(2 * pi * 440 * (off + i) / 48000);
        p.process(in, out, 256);
        for (double s : y) {
            if (off > 48000) maxSecond = std::max(maxSecond, std::abs(s - 2 * last + previous));
            previous = last;
            last = s;
        }
    }
    CHECK(maxSecond < .4);
    CHECK(p.telemetry().timeMs[0] == doctest::Approx(200).epsilon(.002));
    p.setParam(kSync, 12);
    p.setTempo(60);
    p.process(in, out, 1);
    CHECK(p.telemetry().timeMs[0] < 201);
    for (int i = 0; i < 400; ++i) p.process(in, out, 256);
    CHECK(p.telemetry().timeMs[0] == doctest::Approx(750).epsilon(.001));
}
TEST_CASE("Delay feedback self-oscillation is bounded and sustained for thirty seconds") {
    auto v = wet(25);
    v[kFeedback] = 110;
    v[kHighpass] = 80;
    v[kLowpass] = 8000;
    Processor p;
    REQUIRE(p.prepare(48000, 256));
    p.setParams(v);
    Rng rng(17);
    double x[256]{}, l[256]{}, r[256]{};
    const double* in[]{x, x};
    double* out[]{l, r};
    double peak = 0, sum = 0;
    bool finite = true;
    for (int off = 0; off < 30 * 48000; off += 256) {
        for (double& s : x) s = off < 2400 ? (rng.uniform() - .5) * .4 : 0;
        p.process(in, out, 256);
        for (double s : l) {
            finite &= std::isfinite(s);
            peak = std::max(peak, std::abs(s));
            if (off > 29 * 48000) sum += s * s;
        }
    }
    CHECK(finite);
    CHECK(peak <= 1);
    CHECK(std::sqrt(sum / 48000) > std::pow(10., -30. / 20));
    CHECK(safetyLimit(.5) == .5);
    CHECK(safetyLimit(-.2) == -.2);
    CHECK(safetyLimit(1e9) <= 1);
}
TEST_CASE("Delay freeze has stationary energy, rejects new input and resumes smoothly") {
    auto v = wet(23.37);
    v[kFeedback] = 70;
    v[kModDepth] = 30;
    Processor a, b;
    REQUIRE(a.prepare(48000, 256));
    REQUIRE(b.prepare(48000, 256));
    a.setParams(v);
    b.setParams(v);
    double x[256]{}, silence[256]{}, la[256]{}, ra[256]{}, lb[256]{}, rb[256]{};
    Rng rng(33);
    const double* in[]{x, x};
    const double* empty[]{silence, silence};
    double* oa[]{la, ra};
    double* ob[]{lb, rb};
    for (int off = 0; off < 48000; off += 256) {
        for (double& s : x) s = (rng.uniform() - .5) * .1;
        a.process(in, oa, 256);
        b.process(in, ob, 256);
    }
    a.setParam(kFreeze, 1);
    b.setParam(kFreeze, 1);
    for (int off = 0; off < 2048; off += 256) {
        a.process(empty, oa, 256);
        b.process(empty, ob, 256);
    }
    const auto held = a.telemetry().timeMs;
    a.setParam(kTime, 1000);
    a.setTempo(60);
    double early = 0, late = 0;
    bool equal = true;
    for (int off = 0; off < 20 * 48000; off += 256) {
        for (double& s : x) s = .3;
        a.process(in, oa, 256);
        b.process(empty, ob, 256);
        for (int i = 0; i < 256; ++i) {
            equal &= la[i] == lb[i] && ra[i] == rb[i];
            if (off < 48000) early += la[i] * la[i];
            if (off >= 19 * 48000) late += la[i] * la[i];
        }
    }
    CHECK(equal);
    CHECK(std::abs(10 * std::log10(late / early)) < .2);
    CHECK(a.telemetry().timeMs == held);
    a.setParam(kFreeze, 0);
    a.process(empty, oa, 256);
    for (double s : la) CHECK(std::isfinite(s));
}
TEST_CASE("Delay wet duck follows the input and recovers") {
    auto v = wet(20);
    v[kDuck] = 12;
    v[kFeedback] = 0;
    Processor duck, plain;
    REQUIRE(duck.prepare(48000, 256));
    REQUIRE(plain.prepare(48000, 256));
    duck.setParams(v);
    v[kDuck] = 0;
    plain.setParams(v);
    double x[256]{}, a[256]{}, b[256]{}, r[256]{};
    const double* in[]{x, x};
    double* oa[]{a, r};
    double* ob[]{b, r};
    double aa = 0, bb = 0;
    for (int off = 0; off < 96000; off += 256) {
        for (int i = 0; i < 256; ++i) x[i] = .1 * std::sin(2 * pi * 1000 * (off + i) / 48000);
        duck.process(in, oa, 256);
        plain.process(in, ob, 256);
        if (off > 48000)
            for (int i = 0; i < 256; ++i) {
                aa += a[i] * a[i];
                bb += b[i] * b[i];
            }
    }
    CHECK(std::abs(10 * std::log10(aa / bb) + 12) < .5);
    std::fill(std::begin(x), std::end(x), 0);
    for (int i = 0; i < 500; ++i) duck.process(in, oa, 256);
    CHECK(duck.telemetry().duckDb == 0);
}
TEST_CASE("Delay automation stress every key, silence and reset preserve state") {
    Processor p;
    REQUIRE(p.prepare(48000, 64));
    Rng rng(31);
    float x[64]{}, l[64]{}, r[64]{};
    const float* in[]{x, x};
    float* out[]{l, r};
    bool finite = true;
    double peak = 0;
    for (int off = 0; off < 5 * 48000;) {
        const int n = 1 + int(rng.uniform() * 63);
        for (size_t i = 0; i < kNumParams; ++i) {
            const auto& s = table()[i];
            p.setParam(i, s.min + rng.uniform() * (s.max - s.min));
        }
        p.setTempo(40 + 200 * rng.uniform());
        for (int i = 0; i < n; ++i) x[i] = float((rng.uniform() - .5) * .02);
        p.process(in, out, uint32_t(n));
        for (int i = 0; i < n; ++i) {
            finite &= std::isfinite(l[i]) && std::isfinite(r[i]);
            peak = std::max(peak, double(std::max(std::abs(l[i]), std::abs(r[i]))));
        }
        off += n;
    }
    CHECK(finite);
    CHECK(peak < 100);
    p.setParam(kMix, 0);
    p.reset();
    std::fill(std::begin(x), std::end(x), 0);
    p.process(in, out, 64);
    CHECK(std::all_of(std::begin(l), std::end(l), [](float s) { return s == 0; }));
}
TEST_CASE("Delay tail estimates honor feedback, freeze and rate") {
    auto v = wet(100);
    v[kFeedback] = 0;
    CHECK(tailSamples(v, 48000) == 7200);
    v[kFeedback] = 50;
    CHECK(tailSamples(v, 48000) == 69600);
    v[kOffset] = 50;
    CHECK(tailSamples(v, 48000) == 103200);
    v[kFeedback] = 100;
    CHECK(tailSamples(v, 48000) == uint32_t(INT32_MAX));
    v[kFeedback] = 110;
    CHECK(tailSamples(v, 48000) == uint32_t(INT32_MAX));
    v[kFeedback] = 0;
    v[kFreeze] = 1;
    CHECK(tailSamples(v, 48000) == uint32_t(INT32_MAX));
}

TEST_CASE("Delay causal validator rates and maximum line allocation") {
    for (double fs : {1000., 1234.57, 8000., 768000.}) {
        Processor p;
        REQUIRE(p.prepare(fs, 64));
        auto v = wet(1);
        v[kOffset] = -50;
        v[kModDepth] = 100;
        p.setParams(v);
        float x[64]{}, y[64]{}, r[64]{};
        x[0] = .1f;
        const float* in[]{x, x};
        float* out[]{y, r};
        p.process(in, out, 64);
        for (float value : y) CHECK(std::isfinite(value));
    }
}

TEST_CASE("Delay loop filters agree with the digital response across sample rates") {
    for (double fs : {44100., 48000., 88200., 96000., 192000.})
        for (double hz : {50., 1000., 10000.}) {
            auto v = wet(10);
            v[kFeedback] = 0;
            v[kHighpass] = 120;
            v[kLowpass] = 6500;
            Processor p;
            REQUIRE(p.prepare(fs, 256));
            p.setParams(v);
            double x[256]{}, y[256]{}, r[256]{};
            const double* in[]{x, x};
            double* out[]{y, r};
            double xx = 0, yy = 0;
            const int settle = int(fs * .5);
            for (int off = 0; off < 2 * settle; off += 256) {
                for (int i = 0; i < 256; ++i) x[i] = .01 * std::sin(2 * pi * hz * (off + i) / fs);
                p.process(in, out, 256);
                if (off >= settle)
                    for (int i = 0; i < 256; ++i) {
                        xx += x[i] * x[i];
                        yy += y[i] * y[i];
                    }
            }
            const double target = 20 * std::log10(loopMagnitude(hz, 120, 6500, fs));
            CHECK(std::abs(10 * std::log10(yy / xx) - target) < .05);
        }
}
