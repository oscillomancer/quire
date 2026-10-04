#include <doctest/doctest.h>
#include <cmath>
#include <numbers>
#include "supple/analysis/live.h"

using namespace supple;
TEST_CASE("live meter agrees with offline loudness and stereo") {
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        const auto n = static_cast<std::size_t>(fs * 3);
        std::vector<float> l(n), r(n);
        for (std::size_t i = 0; i < n; ++i) {
            l[i] = static_cast<float>(
                std::pow(10., -23. / 20) *
                std::sin(2 * std::numbers::pi * 1000 * static_cast<double>(i) / fs));
            r[i] = l[i];
        }
        const float* in[]{l.data(), r.data()};
        analysis::LiveMeter live;
        live.prepare(fs, 2, 3);
        live.process(in, static_cast<int>(n));
        analysis::Options options;
        options.spectral = false;
        options.harmonic = false;
        options.envelope = false;
        options.monoLoss = false;
        auto offline = analysis::analyze(
            {in, static_cast<std::int64_t>(n), fs, analysis::Layout::Stereo}, options);
        REQUIRE(offline);
        auto m = live.measurements();
        CHECK(std::abs(m.integrated - offline->loudness->integrated) < .1);
        CHECK(std::abs(m.truePeak - offline->loudness->truePeak) < .02);
        CHECK(std::abs(m.correlation - offline->stereo->correlation) < 1e-10);
        CHECK(std::abs(m.centroid - 1000) < 15);
        CHECK(m.width == doctest::Approx(0));
    }
}
TEST_CASE("rolling meters expire old energy and peaks in both modes") {
    constexpr double fs = 48000;
    std::vector<float> a(4800), b(4800);
    for (auto mode :
         {analysis::LoudnessMeter::Mode::Exact, analysis::LoudnessMeter::Mode::Histogram}) {
        analysis::LoudnessMeter meter;
        meter.prepare(fs, analysis::Layout::Stereo, mode, 0, 3);
        for (int hop = 0; hop < 70; ++hop) {
            for (int i = 0; i < 4800; ++i)
                a[static_cast<std::size_t>(i)] = b[static_cast<std::size_t>(i)] =
                    static_cast<float>(
                        (hop < 20 ? .8 : .08) *
                        std::sin(2 * std::numbers::pi * 1000 * (hop * 4800 + i) / fs));
            const float* in[]{a.data(), b.data()};
            meter.process(in, 4800);
        }
        CHECK(meter.truePeak() < -20);
        CHECK(meter.integrated() < -20);
        CHECK(meter.blockCount() == 27);
        CHECK(meter.loudnessRange() < .02);
        meter.reset();
        CHECK(std::isinf(meter.integrated()));
    }
}
TEST_CASE("live silence and anti-phase keep finite report values") {
    analysis::LiveMeter meter;
    meter.prepare(48000, 2, 0);
    std::vector<float> l(48000), r(48000);
    const float* in[]{l.data(), r.data()};
    meter.process(in, 48000);
    auto silence = meter.measurements();
    CHECK(silence.integrated == -120);
    CHECK(silence.correlation == 1);
    meter.reset();
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = static_cast<float>(.2 * std::sin(static_cast<double>(i) * .13));
        r[i] = -l[i];
    }
    meter.process(in, 48000);
    auto m = meter.measurements();
    CHECK(m.correlation == doctest::Approx(-1));
    CHECK(m.width == doctest::Approx(1));
    CHECK(m.centroid == 0);
    auto json = analysis::toJson(m, 48000, 2);
    CHECK(json.find("schema")->asString() == "supple.analysis-report/1.0");
    REQUIRE(json.find("spectral"));
    CHECK(json.find("spectral")->find("ltas_db")->asArray().size() == 31);
}

TEST_CASE("an unprepared live meter ignores audio") {
    analysis::LiveMeter meter;
    const float sample = 0.5f;
    const float* input[]{&sample, &sample};
    meter.process(input, 1);
    CHECK(meter.measurements().seconds == 0);
}
