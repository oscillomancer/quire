#include <doctest/doctest.h>
#include <cstdio>
#include <numbers>
#include "comp/processor.h"
#include "supple/analysis/loudness.h"
#include "supple/base/rng.h"

using namespace supple;
using namespace supple::comp;
namespace {
using Audio = std::array<std::vector<float>, 2>;
Audio silence(std::size_t n) { return {std::vector<float>(n), std::vector<float>(n)}; }
Audio sine(double sr, double seconds, double amplitude, double hz) {
    auto a = silence(static_cast<std::size_t>(sr * seconds));
    for (std::size_t i = 0; i < a[0].size(); ++i)
        a[0][i] = a[1][i] = static_cast<float>(
            amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / sr));
    return a;
}
struct Event {
    std::size_t time, id;
    double value;
};
Audio render(const Audio& a, Values v, double sr = 48000, std::size_t block = 64,
             const Audio* key = nullptr, std::span<const Event> events = {}) {
    Processor p;
    REQUIRE(p.prepare(sr, 4096));
    for (std::size_t i = 0; i < kGr; ++i) p.setParam(i, v[i]);
    auto out = silence(a[0].size());
    std::size_t pos = 0, e = 0;
    while (pos < a[0].size()) {
        while (e < events.size() && events[e].time == pos) {
            p.setParam(events[e].id, events[e].value);
            ++e;
        }
        std::size_t n = std::min(block, a[0].size() - pos);
        if (e < events.size()) n = std::min(n, events[e].time - pos);
        const float* in[]{a[0].data() + pos, a[1].data() + pos};
        float* output[]{out[0].data() + pos, out[1].data() + pos};
        const float* k[]{key ? (*key)[0].data() + pos : nullptr,
                         key ? (*key)[1].data() + pos : nullptr};
        p.process(in, output, static_cast<std::uint32_t>(n), key ? k : nullptr);
        pos += n;
    }
    return out;
}
double rms(const std::vector<float>& a, std::size_t start) {
    double energy = 0;
    for (std::size_t i = start; i < a.size(); ++i)
        energy += static_cast<double>(a[i]) * static_cast<double>(a[i]);
    return std::sqrt(energy / static_cast<double>(a.size() - start));
}
double db(double x) { return 20 * std::log10(std::max(x, 1e-12)); }
double tp(const Audio& a, double sr) {
    analysis::LoudnessMeter m;
    m.prepare(sr, analysis::Layout::Stereo, analysis::LoudnessMeter::Mode::Histogram);
    const float* p[]{a[0].data(), a[1].data()};
    m.process(p, static_cast<int>(a[0].size()));
    return m.truePeak();
}
}  // namespace
TEST_CASE("comp table and canonical patches") {
    REQUIRE(params::validateTable(table()));
    CHECK(table().size() == kNumParams);
    for (std::size_t i = 0; i < kNumParams; ++i) {
        CHECK(table()[i].id == i);
        if (table()[i].type == params::Type::Float) CHECK(table()[i].smoothingMs == 20);
    }
    for (auto i : {kMode, kLookahead, kTruePeak})
        CHECK((table()[i].flags & params::kAutomatable) == 0);
    const auto& type = patchType();
    auto patch = type.defaults();
    CHECK(type.schemaId() == "supple.comp-patch/1.0");
    CHECK_FALSE(type.codec().hasSeed());
    auto text = type.serialize(patch);
    auto parsed = type.parse(text);
    REQUIRE(parsed);
    CHECK(type.serialize(*parsed) == text);
    for (std::size_t i = 0; i < kGr; ++i)
        patch.body.values[i] = table()[i].max == table()[i].def ? table()[i].min : table()[i].max;
    text = type.serialize(patch);
    parsed = type.parse(text);
    REQUIRE(parsed);
    CHECK(type.serialize(*parsed) == text);
    auto merge = json::parse(R"({"params":{"threshold":-100,"sidechain":"missing"}})");
    REQUIRE(merge);
    auto bad = type.merge(patch, *merge);
    REQUIRE_FALSE(bad);
    CHECK_FALSE((bad.error().path.empty() && bad.error().details.empty()));
    CHECK(describe(defaults()).empty());
    auto v = defaults();
    v[kThreshold] = -24;
    v[kRatio] = 10;
    CHECK(describe(v).find("dB") != std::string::npos);
    CHECK(describe(v).size() <= 300);
    v.fill(1);
    CHECK(describe(v).size() <= 300);
}
TEST_CASE("comp below threshold and lookahead null at all rates") {
    for (double sr : {22050., 44100., 48000., 88200., 96000., 192000., 384000.}) {
        CAPTURE(sr);
        auto a = sine(sr, .03, .01, 1000);
        for (double ahead : {0., 1., 2.}) {
            auto v = defaults();
            v[kLookahead] = ahead;
            const auto delay = latencyFor(v, sr);
            auto out = render(a, v, sr);
            for (std::size_t i = 0; i < a[0].size(); ++i)
                CHECK(out[0][i] ==
                      doctest::Approx(i < delay ? 0 : static_cast<double>(a[0][i - delay]))
                          .epsilon(1e-6)
                          .scale(1));
        }
    }
}
TEST_CASE("comp static RMS sine curve includes the quadratic knee at all rates") {
    for (double sr : {44100., 48000., 88200., 96000., 192000.})
        for (double level : {-60., -24., -21., -18., -15., -6., 0.}) {
            CAPTURE(sr);
            CAPTURE(level);
            auto v = defaults();
            v[kDetector] = 1;
            v[kAttack] = 10;
            v[kRelease] = 10;
            auto a = sine(sr, .3, std::pow(10., (level + 3.01029995664) / 20), 4000);
            auto out = render(a, v, sr);
            const auto start = static_cast<std::size_t>(sr * .2);
            CHECK(std::abs(db(rms(out[0], start) / rms(a[0], start)) -
                           gainReduction(level, -18, 4, 6)) < .2);
        }
    CHECK(gainReduction(-6, -18, 100, 0) == -12);
}
TEST_CASE("comp attack and release reach 63 percent in the set time") {
    for (double sr : {44100., 48000., 96000., 192000.}) {
        CAPTURE(sr);
        Processor p;
        REQUIRE(p.prepare(sr, 1));
        p.setParam(kThreshold, -30);
        p.setParam(kKnee, 0);
        p.setParam(kAttack, 50);
        p.setParam(kRelease, 100);
        float l = .01f, r = .01f;
        const float* in[]{&l, &r};
        float ol = 0, orr = 0;
        float* out[]{&ol, &orr};
        for (int i = 0; i < static_cast<int>(sr * .1); ++i) p.process(in, out, 1);
        l = r = .1f;
        for (int i = 0; i < static_cast<int>(std::lround(sr * .05)); ++i) p.process(in, out, 1);
        CHECK(p.reduction() == doctest::Approx(-7.5 * (1 - std::exp(-1))).epsilon(.1));
        for (int i = 0; i < static_cast<int>(sr); ++i) p.process(in, out, 1);
        const double before = p.reduction();
        l = r = .01f;
        for (int i = 0; i < static_cast<int>(std::lround(sr * .1)); ++i) p.process(in, out, 1);
        CHECK(p.reduction() == doctest::Approx(before * std::exp(-1)).epsilon(.1));
    }
}
TEST_CASE("comp external key missing and silent, key filters and stereo link") {
    auto v = defaults();
    v[kSidechain] = 1;
    v[kThreshold] = -18;
    v[kAttack] = .01;
    v[kRelease] = 5;
    auto a = sine(48000, .3, .5, 1000);
    auto zero = silence(a[0].size());
    CHECK(render(a, v) == a);
    CHECK(render(a, v, 48000, 64, &zero) == a);
    CHECK(render(zero, v, 48000, 64, &a) == zero);
    v[kScHighpass] = 200;
    auto low = sine(48000, .3, .2, 50), high = sine(48000, .3, .2, 1000);
    auto lowOut = render(a, v, 48000, 64, &low), highOut = render(a, v, 48000, 64, &high);
    CHECK(db(rms(lowOut[0], 9600) / rms(a[0], 9600)) > -1);
    CHECK(db(rms(highOut[0], 9600) / rms(a[0], 9600)) < -1.5);
    v[kScHighpass] = 10;
    v[kScLowpass] = 1000;
    auto veryHigh = sine(48000, .3, .2, 10000);
    auto hp = render(a, v, 48000, 64, &veryHigh);
    CHECK(db(rms(hp[0], 9600) / rms(a[0], 9600)) > -1);
    v[kScLowpass] = 20000;
    v[kStereoLink] = 0;
    auto key = high;
    key[1] = zero[1];
    auto independent = render(a, v, 48000, 64, &key);
    CHECK(independent[1] == a[1]);
    v[kStereoLink] = 100;
    auto linked = render(a, v, 48000, 64, &key);
    CHECK(linked[0] == linked[1]);
}
TEST_CASE("comp lookahead lets reduction precede a delayed transient") {
    auto a = silence(1000);
    for (std::size_t i = 400; i < 500; ++i) a[0][i] = a[1][i] = 1;
    auto v = defaults();
    v[kAttack] = 1;
    v[kKnee] = 0;
    v[kLookahead] = 2;
    auto out = render(a, v);
    const auto d = latencyFor(v, 48000);
    CHECK(d == 240);
    CHECK(out[0][400 + d] < .3f);
    CHECK(out[0][399 + d] == 0);
    v[kLookahead] = 0;
    auto no = render(a, v);
    CHECK(no[0][400] > .9f);
}
TEST_CASE("comp feedback settles with negative feedback and auto makeup matches reference") {
    auto a = silence(48000);
    for (auto& ch : a) std::fill(ch.begin(), ch.end(), 1.f);
    auto v = defaults();
    v[kThreshold] = -24;
    v[kKnee] = 0;
    v[kTopology] = 1;
    v[kAttack] = 10;
    v[kRelease] = 10;
    auto out = render(a, v);
    CHECK(std::abs(db(rms(out[0], 40000)) + 24 * 3. / 7) < .1);
    v = defaults();
    v[kThreshold] = -30;
    v[kKnee] = 0;
    v[kAutoMakeup] = 1;
    v[kDetector] = 1;
    v[kRelease] = 10;
    a = sine(48000, .5, std::pow(10., (-18 + 3.01029995664) / 20), 4000);
    out = render(a, v);
    CHECK(std::abs(db(rms(out[0], 20000) / rms(a[0], 20000))) < .2);
}
TEST_CASE("comp block partition and meter observation cannot change audio") {
    Rng rng(12);
    auto a = silence(10000), key = silence(10000);
    for (std::size_t i = 0; i < a[0].size(); ++i)
        for (std::size_t c = 0; c < 2; ++c) {
            a[c][i] = rng.bipolar() * .5f;
            key[c][i] = rng.bipolar();
        }
    const Event events[] = {{3, kThreshold, -30},    {51, kRatio, 10},       {79, kDetector, 1},
                            {210, kScHighpass, 200}, {701, kSidechain, 1},   {705, kTopology, 1},
                            {1300, kMix, 50},        {4001, kOutput, -3},    {4503, kScListen, 1},
                            {5001, kScListen, 0},    {7500, kAutoRelease, 1}};
    for (bool limiter : {false, true}) {
        auto v = defaults();
        v[kMode] = limiter ? 1 : 0;
        v[kLookahead] = 2;
        const auto reference = render(a, v, 48000, 1, &key, events);
        for (std::size_t b : {7u, 64u, 4096u})
            CHECK(render(a, v, 48000, b, &key, events) == reference);
    }
    Processor p, q;
    REQUIRE(p.prepare(48000, 64));
    REQUIRE(q.prepare(48000, 64));
    float a1 = .5f, a2 = .2f, o1 = 0, o2 = 0;
    const float* in[]{&a1, &a2};
    float* out1[]{&o1, &o2};
    float z1 = 0, z2 = 0;
    float* out2[]{&z1, &z2};
    for (int i = 0; i < 1000; ++i) {
        p.process(in, out1, 1);
        q.process(in, out2, 1);
        if (i % 7 == 0) p.takeTelemetry();
        CHECK(o1 == z1);
        CHECK(o2 == z2);
    }
}
TEST_CASE("comp randomized saved state gives identical fresh output") {
    Rng r(19);
    auto patch = patchType().defaults();
    Values v = defaults();
    for (std::size_t i = 0; i < kGr; ++i) {
        v[i] = params::fromNormalized(table()[i], r.uniform());
        patch.body.values[i] = v[i];
    }
    auto restored = patchType().parse(patchType().serialize(patch));
    REQUIRE(restored);
    Values w{};
    std::copy(restored->body.values.begin(), restored->body.values.end(), w.begin());
    auto a = sine(48000, .05, .5, 2000);
    CHECK(render(a, v) == render(a, w));
}
TEST_CASE("comp five second full parameter stress is finite and below plus 40 dBFS") {
    Rng r(21);
    Processor p;
    REQUIRE(p.prepare(48000, 64));
    bool good = true;
    float peak = 0;
    std::uint32_t remain = 0;
    for (int i = 0; i < 240000; ++i) {
        if (remain == 0) {
            for (std::size_t id = 0; id < kGr; ++id)
                p.setParam(id, params::fromNormalized(table()[id], r.uniform()));
            remain = 1 + static_cast<std::uint32_t>(r.next() % 64);
        }
        --remain;
        float l = r.bipolar() * .1f, rr = r.bipolar() * .1f, k = r.bipolar();
        const float* in[]{&l, &rr};
        const float* key[]{&k, &k};
        float ol = 0, orr = 0;
        float* out[]{&ol, &orr};
        p.process(in, out, 1, key);
        good = good && std::isfinite(ol) && std::isfinite(orr);
        peak = std::max({peak, std::abs(ol), std::abs(orr)});
    }
    CHECK(good);
    CHECK(peak < 100);
}
TEST_CASE("comp limiter below ceiling null and latency at all rates") {
    for (double sr : {22050., 44100., 48000., 88200., 96000., 192000., 384000.})
        for (double tpOn : {0., 1.})
            for (double ahead : {0., 1., 2.}) {
                CAPTURE(sr);
                CAPTURE(tpOn);
                CAPTURE(ahead);
                auto v = defaults();
                v[kMode] = 1;
                v[kTruePeak] = tpOn;
                v[kLookahead] = ahead;
                auto a = sine(sr, .04, std::pow(10., -.5), 1000);
                const auto d = latencyFor(v, sr);
                auto out = render(a, v, sr);
                CHECK(d == (ahead == 0 ? 0
                                       : static_cast<std::uint32_t>(
                                             std::lround(sr * (ahead == 1 ? .001 : .005)))) +
                               (tpOn > 0 ? 16u : 0u));
                for (std::size_t i = 0; i < a[0].size(); ++i)
                    CHECK(std::abs(out[0][i] - (i < d ? 0 : a[0][i - d])) < 1e-6f);
            }
}
TEST_CASE("comp limiter attack follows the lookahead moving average without a gain step") {
    constexpr double sr = 48000;
    constexpr std::size_t step = 480;
    for (double ahead : {1., 2.}) {
        auto v = defaults();
        v[kMode] = 1;
        v[kTruePeak] = 0;
        v[kLookahead] = ahead;
        v[kCeiling] = 0;
        const auto delay = latencyFor(v, sr);
        const auto length = delay + 1;
        auto a = silence(step + delay + 100);
        for (auto& ch : a) {
            std::fill(ch.begin(), ch.end(), .5f);
            std::fill(ch.begin() + step, ch.end(), 2.f);
        }
        const auto out = render(a, v, sr);
        double previous = 1, maximumDrop = 0;
        for (std::size_t i = step; i <= step + delay; ++i) {
            const double applied =
                static_cast<double>(out[0][i]) / static_cast<double>(a[0][i - delay]);
            CHECK(applied <= previous + 1e-7);
            maximumDrop = std::max(maximumDrop, previous - applied);
            CHECK(previous - applied <= 1.05 * .5 / length);
            previous = applied;
        }
        CHECK(previous == doctest::Approx(.5).epsilon(1e-7));
        std::printf("limiter ramp: %u samples, max gain drop %.9f, allowed %.9f, arrival %.9f\n",
                    length, maximumDrop, 1.05 * .5 / length, previous);
    }
}
TEST_CASE("comp limiter ignores compressor controls including during key audition") {
    auto a = sine(48000, .08, .5, 1000);
    for (double listen : {0., 1.}) {
        auto v = defaults();
        v[kMode] = 1;
        v[kLookahead] = 1;
        v[kScListen] = listen;
        v[kScHighpass] = 200;
        const auto expected = render(a, v);
        v[kThreshold] = -60;
        v[kRatio] = 100;
        v[kKnee] = 24;
        v[kAttack] = .01;
        v[kAutoRelease] = 1;
        v[kDetector] = 1;
        v[kRmsWindow] = 100;
        v[kTopology] = 1;
        v[kAutoMakeup] = 1;
        CHECK(render(a, v) == expected);
    }
}
TEST_CASE("comp true peak limiter preserves a minus three dB sine below a minus one ceiling") {
    for (double sr : {22050., 44100., 48000., 88200., 96000., 192000., 384000.})
        for (double ahead : {1., 2.}) {
            auto v = defaults();
            v[kMode] = 1;
            v[kLookahead] = ahead;
            auto a = sine(sr, .08, std::pow(10., -3. / 20), 1000);
            const auto out = render(a, v, sr);
            const auto delay = latencyFor(v, sr);
            double error = 0;
            for (std::size_t i = 0; i < a[0].size(); ++i)
                error = std::max(error,
                                 std::abs(static_cast<double>(out[0][i]) -
                                          (i < delay ? 0 : static_cast<double>(a[0][i - delay]))));
            CHECK(error < 1e-6);
            if (sr == 48000 && ahead == 1)
                std::printf("limiter -3 dB sine: delayed error %.9g, input %.6f dBTP\n", error,
                            tp(a, sr));
        }
}
TEST_CASE("comp true peak pink noise ceiling retains usable sample peak headroom") {
    for (double sr : {44100., 48000., 96000.}) {
        auto a = silence(static_cast<std::size_t>(sr * 2));
        Rng r(67);
        std::array<double, 12> rows{};
        double inputPeak = 0;
        for (std::size_t i = 0; i < a[0].size(); ++i) {
            double sample = static_cast<double>(r.bipolar());
            // Equal-energy octave bands, with independent held random values.
            for (std::size_t j = 0; j < rows.size(); ++j) {
                if (i % (std::size_t{1} << j) == 0) rows[j] = static_cast<double>(r.bipolar());
                sample += rows[j];
            }
            a[0][i] = a[1][i] = static_cast<float>(sample);
            inputPeak = std::max(inputPeak, std::abs(sample));
        }
        for (auto& ch : a)
            for (auto& sample : ch)
                sample = static_cast<float>(static_cast<double>(sample) / inputPeak);
        for (double ahead : {1., 2.}) {
            auto v = defaults();
            v[kMode] = 1;
            v[kLookahead] = ahead;
            v[kOutput] = 12;
            const auto out = render(a, v, sr);
            double peak = 0;
            for (float sample : out[0])
                peak = std::max(peak, std::abs(static_cast<double>(sample)));
            const double truePeak = tp(out, sr), samplePeak = db(peak);
            CAPTURE(sr);
            CAPTURE(ahead);
            CAPTURE(samplePeak);
            CAPTURE(truePeak);
            CHECK(samplePeak >= -1.6);
            CHECK(truePeak <= -.9);
            std::printf("limiter pink +12 dB: %.0f Hz, %.0f ms, %.6f dBFS, %.6f dBTP\n", sr,
                        ahead == 1 ? 1. : 5., samplePeak, truePeak);
        }
    }
}
TEST_CASE("comp limiter ceiling on bursts squares noise and intersample peaks") {
    for (double sr : {44100., 48000., 88200., 96000., 192000.})
        for (int kind = 0; kind < 5; ++kind) {
            CAPTURE(sr);
            CAPTURE(kind);
            auto a = silence(static_cast<std::size_t>(sr * .06));
            Rng r(1);
            for (std::size_t i = 50; i < a[0].size() - 100; ++i) {
                const double t = static_cast<double>(i) / sr;
                const double s =
                    kind == 0
                        ? 4 * std::sin(2 * std::numbers::pi * 1000 * t)
                        : (kind == 1
                               ? (i % 20 < 10 ? 10 : -10)
                               : (kind == 2
                                      ? 10 * static_cast<double>(r.bipolar())
                                      : (kind == 3 ? 1.4 * std::sin(std::numbers::pi *
                                                                        static_cast<double>(i) / 2 +
                                                                    std::numbers::pi / 4)
                                                   : 3 * (std::sin(t * 2200) +
                                                          .3 * static_cast<double>(r.bipolar())))));
                a[0][i] = static_cast<float>(s);
                a[1][i] = static_cast<float>(s * .7);
            }
            for (double truePeak : {0., 1.}) {
                auto v = defaults();
                v[kMode] = 1;
                v[kLookahead] = 1;
                v[kCeiling] = -3;
                v[kTruePeak] = truePeak;
                auto out = render(a, v, sr);
                float peak = 0;
                for (const auto& ch : out)
                    for (float x : ch) peak = std::max(peak, std::abs(x));
                CHECK(db(static_cast<double>(peak)) <= -3 + 1e-5);
                if (truePeak > 0) CHECK(tp(out, sr) <= -3 + .1);
            }
        }
}
TEST_CASE("comp limiter recovers after five release times") {
    auto a = silence(48000);
    for (auto& ch : a) {
        std::fill(ch.begin(), ch.end(), .1f);
        std::fill(ch.begin() + 480, ch.begin() + 960, 4.f);
    }
    auto v = defaults();
    v[kMode] = 1;
    v[kRelease] = 50;
    v[kLookahead] = 1;
    auto out = render(a, v);
    CHECK(db(static_cast<double>(out[0][14000]) / .1) > -.1);
}
TEST_CASE("comp prepare restore reset mono double in-place and tiny silence") {
    Processor p;
    REQUIRE(p.prepare(48000, 64, 1));
    p.setParam(kRatio, 1);
    p.setParam(kOutput, 6);
    double a[64];
    std::fill(std::begin(a), std::end(a), .01);
    const double* in[]{a};
    double* out[]{a};
    p.process(in, out, 64);
    CHECK(a[0] == doctest::Approx(.01 * std::pow(10., .3)).epsilon(1e-12));
    REQUIRE(p.prepare(48000, 64, 1));
    std::fill(std::begin(a), std::end(a), .01);
    p.process(in, out, 64);
    CHECK(a[0] == doctest::Approx(.01 * std::pow(10., .3)).epsilon(1e-12));
    p.reset();
    p.setParam(kOutput, 0);
    std::fill(std::begin(a), std::end(a), 1e-35);
    p.process(in, out, 64);
    CHECK(std::isfinite(a[63]));
}
TEST_CASE("comp auto release is slower after sustained reduction, finite silence tail") {
    auto a = silence(144000);
    for (auto& channel : a) std::fill(channel.begin(), channel.begin() + 96000, 1.f);
    auto v = defaults();
    v[kAttack] = .01;
    v[kRelease] = 100;
    v[kKnee] = 0;
    auto manual = render(a, v);
    v[kAutoRelease] = 1;
    auto automatic = render(a, v);
    // A quiet probe makes the release gain observable without feeding the detector.
    auto key = a;
    for (auto& channel : a) std::fill(channel.begin() + 96000, channel.end(), .01f);
    v[kSidechain] = 1;
    v[kAutoRelease] = 0;
    manual = render(a, v, 48000, 64, &key);
    v[kAutoRelease] = 1;
    automatic = render(a, v, 48000, 64, &key);
    CHECK(automatic[0][100800] < manual[0][100800]);
    bool finite = true;
    for (float sample : automatic[0]) finite = finite && std::isfinite(sample);
    CHECK(finite);
}
TEST_CASE("comp stereo limiter protects unequal channels with dry mix and output drive") {
    auto a = sine(48000, .1, 4, 12000);
    for (auto& sample : a[1]) sample *= .2f;
    auto v = defaults();
    v[kMode] = 1;
    v[kLookahead] = 1;
    v[kMix] = 0;
    v[kOutput] = 24;
    v[kStereoLink] = 0;
    v[kCeiling] = -6;
    auto out = render(a, v);
    CHECK(tp(out, 48000) <= -6 + .1);
    v[kMix] = 50;
    v[kMakeup] = 24;
    out = render(a, v);
    CHECK(tp(out, 48000) <= -6 + .1);
}
TEST_CASE("comp reports invalid prepare configurations without throwing") {
    Processor p;
    CHECK_FALSE(p.prepare(0, 64));
    CHECK_FALSE(p.prepare(48000, 0));
    CHECK_FALSE(p.prepare(48000, 64, 3));
    CHECK(p.prepare(48000, 64));
}
TEST_CASE("comp mono still detects the right side of a stereo external key") {
    Processor p;
    REQUIRE(p.prepare(48000, 1, 1));
    p.setParam(kSidechain, 1);
    p.setParam(kStereoLink, 0);
    p.setParam(kAttack, .01);
    float input = .5f, keyLeft = 0, keyRight = 1, output = 0;
    const float* in[]{&input};
    float* out[]{&output};
    const float* key[]{&keyLeft, &keyRight};
    for (int i = 0; i < 1000; ++i) p.process(in, out, 1, key);
    CHECK(output < .15f);
}
TEST_CASE("comp ratio smoothing reaches infinity without an endpoint jump") {
    Processor p;
    REQUIRE(p.prepare(48000, 1));
    p.setParam(kThreshold, -60);
    p.setParam(kKnee, 0);
    p.setParam(kAttack, .01);
    p.setParam(kRatio, 99);
    float input = 1, l = 0, r = 0;
    const float* in[]{&input, &input};
    float* out[]{&l, &r};
    for (int i = 0; i < 1000; ++i) p.process(in, out, 1);
    double previous = db(static_cast<double>(l)), maxStep = 0;
    p.setParam(kRatio, 100);
    for (int i = 0; i < 48000; ++i) {
        p.process(in, out, 1);
        const double next = db(static_cast<double>(l));
        maxStep = std::max(maxStep, std::abs(next - previous));
        previous = next;
    }
    CHECK(maxStep < .01);
    CHECK(std::abs(previous + 60) < .001);
}
