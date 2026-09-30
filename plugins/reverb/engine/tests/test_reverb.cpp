#include <doctest/doctest.h>
#include "measurements.h"
#include "supple/base/rng.h"
#include "supple/json/json.h"
using namespace supple;
using namespace supple::reverb;
TEST_CASE("Reverb table, canonical patches, errors and state determinism") {
    REQUIRE(params::validateTable(table()));
    for (std::size_t i = 0; i < kNumParams; ++i) CHECK(table()[i].id == i);
    const auto& type = patchType();
    auto patch = type.defaults();
    auto textPatch = json::parse(R"({"params":{"low_decay":"1.5 x","high_decay":"0.4 x"}})");
    REQUIRE(textPatch);
    auto unitPatch = type.merge(patch, *textPatch);
    REQUIRE(unitPatch);
    CHECK(unitPatch->body.values[kLowDecay] == 1.5);
    CHECK(unitPatch->body.values[kHighDecay] == .4);
    for (auto id : {kLowDecay, kHighDecay}) {
        auto parsed = params::parse(table()[id], formatValue(id, unitPatch->body.values[id]));
        REQUIRE(parsed);
        CHECK(*parsed == unitPatch->body.values[id]);
    }
    for (bool full : {false, true}) {
        if (full)
            for (std::size_t i = 0; i < kNumParams; ++i) patch.body.values[i] = table()[i].max;
        auto text = type.serialize(patch);
        auto parsed = type.parse(text);
        REQUIRE(parsed);
        CHECK(parsed->body == patch.body);
        CHECK(type.serialize(*parsed) == text);
        Values v{};
        std::copy(parsed->body.values.begin(), parsed->body.values.end(), v.begin());
        CHECK(measurements::impulse(v, 48000, .25) == measurements::impulse(v, 48000, .25));
    }
    auto bad = json::parse(R"({"params":{"decay":0}})");
    REQUIRE(bad);
    auto merged = type.merge(patch, *bad);
    REQUIRE_FALSE(merged);
    CHECK(merged.error().path == "/params/decay");
    CHECK(describe(defaults()).size() <= 300);
}
TEST_CASE("Reverb defaults and sample-offset automation are bit invariant") {
    auto render = [](int block, bool automation) {
        Processor p;
        REQUIRE(p.prepare(48000, 4096));
        measurements::Stereo y{std::vector<double>(16000), std::vector<double>(16000)};
        std::vector<double> x(16000);
        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] = .1 * std::sin(static_cast<double>(i) * .075);
        for (std::size_t off = 0; off < x.size();) {
            if (automation && off % 997 == 0) {
                p.setParam(kSize, static_cast<double>(off % 101));
                p.setParam(kDecay, 100 + static_cast<double>(off));
                p.setParam(kPredelay, static_cast<double>(off % 125));
                p.setParam(kFreeze, off / 997 % 4 == 1 ? 1 : 0);
            }
            auto n = std::min(static_cast<std::size_t>(block), x.size() - off);
            if (automation) n = std::min(n, 997 - off % 997);
            const double* in[] = {x.data() + off, x.data() + off};
            double* out[] = {y[0].data() + off, y[1].data() + off};
            p.process(in, out, static_cast<std::uint32_t>(n));
            off += n;
        }
        return y;
    };
    for (bool events : {false, true}) {
        auto ref = render(1, events);
        for (int block : {7, 64, 4096}) CHECK(render(block, events) == ref);
    }
}
TEST_CASE("Reverb RT60 octave T30 measurements") {
    double worst = 0;
    std::array<double, 7> bands{};
    for (int shape : {0, 1})
        for (double fs : {44100., 48000., 88200., 96000., 192000.})
            for (double decay : {.5, 2., 8.})
                for (double size : {20., 50., 90.}) {
                    auto v = defaults();
                    if (shape == 1) {
                        v[kCrossover] = 1000;
                        v[kLowDecay] = 1.5;
                        v[kDampingFreq] = 2000;
                    }
                    v[kDecay] = decay * 1000;
                    v[kSize] = size;
                    v[kModulation] = 0;
                    v[kEarly] = -60;
                    v[kMix] = 100;
                    auto ir = measurements::impulse(v, fs, decay * 1.5 + .5);
                    for (double f : {125., 250., 500., 1000., 2000., 4000., 8000.}) {
                        const double t = measurements::rt60(ir, f, fs);
                        if (f > v[kCrossover] * .5 && f < v[kCrossover] * 2) continue;
                        if (f > v[kDampingFreq] * .5 && f < v[kDampingFreq] * 2) continue;
                        const double target = decay * (f <= v[kCrossover] * .5    ? v[kLowDecay]
                                                       : f >= v[kDampingFreq] * 2 ? v[kHighDecay]
                                                                                  : 1);
                        const double error = std::abs(t / target - 1);
                        worst = std::max(worst, error);
                        bands[static_cast<std::size_t>(std::lround(std::log2(f / 125)))] = std::max(
                            bands[static_cast<std::size_t>(std::lround(std::log2(f / 125)))],
                            error);
                        CAPTURE(fs);
                        CAPTURE(decay);
                        CAPTURE(size);
                        CAPTURE(f);
                        CAPTURE(t);
                        CAPTURE(target);
                        CHECK(error <= .1);
                    }
                }
    for (std::size_t i = 0; i < bands.size(); ++i)
        std::printf("RT60 band %.0f Hz worst %.4f %%\n", 125 * std::exp2(static_cast<double>(i)),
                    100 * bands[i]);
    std::printf("RT60 worst relative error: %.4f %%\n", 100 * worst);
}
TEST_CASE("Reverb echo density and stereo decorrelation") {
    auto v = defaults();
    v[kMix] = 100;
    const auto normal = measurements::impulse(v, 48000, 2.5);
    const double defaultMs = measurements::densityTime(normal[0], 48000);
    v[kDiffusion] = 100;
    const auto dense = measurements::impulse(v, 48000, 2.5);
    const double denseMs = measurements::densityTime(dense[0], 48000);
    std::printf("Echo density >=0.8: default %.1f ms, diffusion100 %.1f ms\n", defaultMs, denseMs);
    CHECK(defaultMs <= 150);
    CHECK(denseMs < defaultMs);
    const double correlation = measurements::correlation(normal, 24000, 96000);
    std::printf("Late-tail correlation: %.6f\n", correlation);
    CHECK(std::abs(correlation) < .3);
    v[kWidth] = 0;
    const auto mono = measurements::impulse(v, 48000, 2.5);
    CHECK(measurements::correlation(mono, 24000, 96000) > .99);
    CHECK(mono[0] == mono[1]);
}
TEST_CASE("Reverb freeze is held, early-off is exact and tail is reported") {
    auto v = defaults();
    CHECK(tailFor(v, 48000) == 149280);
    v[kFreeze] = 1;
    CHECK(tailFor(v, 48000) == UINT32_MAX);
    Processor p;
    REQUIRE(p.prepare(48000, 512));
    p.setParam(kMix, 100);
    p.setParam(kEarly, -60);
    p.setParam(kModulation, 0);
    std::array<double, 512> x{}, l{}, r{};
    x[0] = 1;
    const double* in[] = {x.data(), x.data()};
    double* out[] = {l.data(), r.data()};
    double initial = 0, final = 0;
    for (int block = 0; block < 1125; ++block) {
        if (block == 94) p.setParam(kFreeze, 1);
        p.process(in, out, 512);
        x.fill(0);
        if (block >= 188 && block < 282)
            for (std::size_t j = 0; j < 512; ++j) initial += l[j] * l[j] + r[j] * r[j];
        if (block >= 1031)
            for (std::size_t j = 0; j < 512; ++j) final += l[j] * l[j] + r[j] * r[j];
    }
    const double change = 10 * std::log10(final / initial);
    std::printf("Freeze energy change: %.4f dB\n", change);
    CHECK(std::abs(change) < 1);
    v = defaults();
    v[kMix] = 100;
    v[kEarly] = -60;
    v[kPredelay] = 0;
    auto ir = measurements::impulse(v, 48000, .02);
    for (auto& c : ir)
        for (double sample : c) CHECK(sample == 0);
}
TEST_CASE("Reverb dry null, mono, float double and automation stress") {
    for (double fs : {22050., 44100., 48000., 96000., 192000., 384000.}) {
        Processor p;
        REQUIRE(p.prepare(fs, 64, 1));
        p.setParam(kMix, 0);
        std::array<double, 64> x{};
        for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::sin(static_cast<double>(i));
        auto expected = x;
        const double* in[] = {x.data()};
        double* out[] = {x.data()};
        p.process(in, out, 64);
        CHECK(x == expected);
        CHECK(p.latency() == 0);
    }
    Processor p;
    REQUIRE(p.prepare(48000, 64));
    Rng rng(42);
    std::array<double, 64> x{}, l{}, r{};
    const double* in[] = {x.data(), x.data()};
    double* out[] = {l.data(), r.data()};
    for (int off = 0; off < 240000;) {
        const auto n = static_cast<std::uint32_t>(1 + rng.next() % 64);
        for (std::size_t i = 0; i < kNumParams; ++i) {
            const auto& s = table()[i];
            p.setParam(i, s.min + (s.max - s.min) * rng.uniform());
        }
        for (auto& sample : x) sample = (2 * rng.uniform() - 1);
        p.process(in, out, n);
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(std::isfinite(l[i]));
            CHECK(std::isfinite(r[i]));
            CHECK(std::abs(l[i]) < 100);
            CHECK(std::abs(r[i]) < 100);
        }
        off += static_cast<int>(n);
    }
}

TEST_CASE("Reverb predelay moves the entire wet impulse and restored freeze ignores input") {
    auto v = defaults();
    v[kMix] = 100;
    v[kModulation] = 0;
    v[kPredelay] = 0;
    const auto plain = measurements::impulse(v, 48000, 1);
    v[kPredelay] = 25;
    const auto delayed = measurements::impulse(v, 48000, 1.025);
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t i = 0; i < 1200; ++i) CHECK(delayed[c][i] == 0);
        for (std::size_t i = 0; i < plain[c].size(); ++i)
            CHECK(delayed[c][i + 1200] == plain[c][i]);
    }
    v[kFreeze] = 1;
    const auto frozen = measurements::impulse(v, 48000, .25);
    for (const auto& c : frozen)
        for (double x : c) CHECK(x == 0);
    v[kFreeze] = 0;
    v[kPredelay] = 0;
    v[kEarly] = -60;
    auto off = measurements::impulse(v, 48000, .020);
    v[kEarly] = 6;
    auto on = measurements::impulse(v, 48000, .020);
    double energy = 0;
    for (double x : on[0]) energy += x * x;
    CHECK(energy > 0);
    for (double x : off[0]) CHECK(x == 0);
}

TEST_CASE("Reverb modulation preserves high-frequency decay") {
    for (double fs : {44100., 48000.}) {
        auto v = defaults();
        v[kSize] = 20;
        v[kDecay] = 8000;
        v[kMix] = 100;
        v[kEarly] = -60;
        v[kDampingFreq] = 2000;
        const auto ir = measurements::impulse(v, fs, 13);
        const double t = measurements::rt60(ir, 8000, fs);
        CHECK(std::isfinite(t));
        CHECK(std::abs(t / 4 - 1) < .1);
        std::printf("Modulation20 size20 decay8: %.0f Hz, 8kHz RT60 %.6f s (target4s)\n", fs, t);
    }
}
TEST_CASE("Reverb prepare then restore snaps every control before the first sample") {
    auto values = defaults();
    values[kSize] = 85;
    values[kDecay] = 4500;
    values[kPredelay] = 25;
    values[kHighDecay] = .4;
    values[kMix] = 100;
    Processor before, after;
    for (std::size_t i = 0; i < values.size(); ++i) before.setParam(i, values[i]);
    REQUIRE(before.prepare(48000, 512));
    REQUIRE(after.prepare(48000, 512));
    for (std::size_t i = 0; i < values.size(); ++i) after.setParam(i, values[i]);
    std::array<double, 512> input{}, a{}, b{}, c{}, d{};
    input[0] = 1;
    const double* in[] = {input.data(), input.data()};
    double* outA[] = {a.data(), b.data()};
    double* outB[] = {c.data(), d.data()};
    for (int block = 0; block < 40; ++block) {
        before.process(in, outA, 512);
        after.process(in, outB, 512);
        CHECK(a == c);
        CHECK(b == d);
        input.fill(0);
    }
}
