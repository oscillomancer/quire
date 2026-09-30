// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <random>
#include <vector>
#include "drive/processor.h"
#include "supple/analysis/loudness.h"
#include "supple/dsp/dsp.h"
using namespace supple;
using namespace supple::drive;
namespace {
constexpr double pi = std::numbers::pi;
void process(Processor& p, std::vector<double>& x, size_t start = 0, size_t count = 0) {
    double* io[] = {x.data() + start};
    p.process(io, static_cast<int>(count ? count : x.size() - start));
}
std::vector<double> sine(Processor& p, double hz, double peak, double fs, size_t count) {
    std::vector<double> x(count + static_cast<size_t>(fs));
    for (size_t n = 0; n < x.size(); ++n) x[n] = peak * std::sin(2 * pi * hz * n / fs);
    process(p, x);
    return {x.end() - static_cast<ptrdiff_t>(count), x.end()};
}
std::vector<std::complex<double>> spectrum(const std::vector<double>& x) {
    dsp::Fft fft;
    fft.prepare(x.size());
    std::vector<std::complex<double>> s(x.size() / 2 + 1);
    REQUIRE(fft.forward(x, s));
    return s;
}
Processor instance(int curve, double drive, int quality = 2, double bias = 0, double fs = 48000) {
    Processor p;
    p.setParam(kCurve, curve);
    p.setParam(kDrive, drive);
    p.setParam(kOversampling, quality);
    p.setParam(kBias, bias);
    p.setParam(kAutoGain, 0);
    p.prepare(fs, 4096, 1);
    return p;
}
double loudness(const std::vector<double>& x) {
    std::vector<float> f(x.begin(), x.end());
    const float* io[] = {f.data()};
    analysis::LoudnessMeter m;
    m.prepare(48000, analysis::Layout::Mono, analysis::LoudnessMeter::Mode::Exact, 2);
    m.process(io, static_cast<int>(f.size()));
    return m.integrated();
}
std::vector<double> pink() {
    constexpr size_t n = 65536;
    std::mt19937 rng(481);
    std::normal_distribution<double> noise;
    std::vector<double> white(n);
    for (auto& x : white) x = noise(rng);
    dsp::Fft fft;
    fft.prepare(n);
    std::vector<std::complex<double>> bins(n / 2 + 1);
    REQUIRE(fft.forward(white, bins));
    for (size_t i = 0; i < bins.size(); ++i) {
        const double hz = 48000.0 * i / n;
        bins[i] *= hz >= 20 && hz <= 20000 ? 1 / std::sqrt(hz) : 0;
    }
    std::vector<double> out(n);
    REQUIRE(fft.inverse(bins, out));
    double power = 0;
    for (double x : out) power += x * x;
    for (auto& x : out) x /= std::sqrt(power / n);
    return out;
}
}  // namespace
TEST_CASE("drive table, canonical patches, merge errors and compact descriptions") {
    auto validation = params::validateTable(kParams);
    if (!validation)
        std::cout << validation.error().message << " " << validation.error().path << "\n";
    REQUIRE(validation);
    for (size_t i = 0; i < kNumParams; ++i) CHECK(kParams[i].id == i);
    CHECK((kParams[kOversampling].flags & pa::kAutomatable) == 0);
    auto p = patchType().defaults();
    for (bool nondefault : {false, true}) {
        if (nondefault)
            for (int i = 0; i < 8; ++i) p.body.values[i] = kParams[i].max;
        auto s = patchType().serialize(p);
        auto q = patchType().parse(s);
        REQUIRE(q);
        CHECK(p.body == q->body);
        CHECK(patchType().serialize(*q) == s);
    }
    auto error = json::parse(R"({"params":{"drive":37}})");
    REQUIRE(error);
    auto q = patchType().merge(p, *error);
    CHECK_FALSE(q);
    CHECK(q.error().path == "/params/drive");
    Values v = defaults();
    for (int i = 0; i < 8; ++i) v[i] = kParams[i].max;
    CHECK(describe(v).size() <= 300);
}
TEST_CASE("drive antiderivatives and odd symmetry agree with analytic curves") {
    for (int c = 0; c < 6; ++c)
        for (double x : {-.9, -.1, .1, .9, 2., 20.}) {
            const double h = 1e-5;
            CHECK((curveIntegral(c, x + h) - curveIntegral(c, x - h)) / (2 * h) ==
                  doctest::Approx(curveValue(c, x)).epsilon(1e-7));
            CHECK(curveValue(c, -x) == -curveValue(c, x));
            CHECK(curveAdaa(c, x, x) == curveValue(c, x));
            CHECK(curveValue(c, 1e-7) == doctest::Approx(1e-7).epsilon(1e-6));
        }
}
TEST_CASE("drive reported latency matches impulse and dry path across rates and factors") {
    for (double fs : {22050., 44100., 48000., 88200., 96000., 192000., 384000.})
        for (int q = 0; q < 4; ++q) {
            auto p = instance(0, 0, q, 0, fs);
            std::vector<double> x(1024);
            x[0] = 1e-5;
            process(p, x);
            const auto peak =
                static_cast<uint32_t>(std::max_element(x.begin(), x.end()) - x.begin());
            CHECK(peak == p.latency());
            p.reset();
            p.setParam(kMix, 0);
            x.assign(1024, 0);
            x[0] = .5;
            process(p, x);
            CHECK(x[p.latency()] == .5);
            double sum = 0;
            for (double y : x) sum += std::abs(y);
            CHECK(sum == .5);
            p.setParam(kMix, 50);
            p.reset();
            auto y = sine(p, 100, 1e-4, fs, static_cast<size_t>(fs));
            double power = 0;
            for (double z : y) power += z * z;
            CHECK(std::abs(10 * std::log10(2 * power / y.size() / 1e-8)) < .012);
        }
}
TEST_CASE("drive THD rises with drive except fold; bias creates even harmonics and rejects DC") {
    // 1 kHz sine, -18 dBFS RMS (peak sqrt(2)*10^(-18/20)).
    for (int c = 0; c < 6; ++c) {
        double previous = -1;
        for (double d : {0., 6., 12., 18., 24., 30., 36.}) {
            auto p = instance(c, d);
            const auto s = spectrum(sine(p, 1000, std::sqrt(2.) * std::pow(10., -.9), 48000, 4800));
            double odd = 0, even = 0;
            for (int h = 2; h <= 20; ++h) (h % 2 ? odd : even) += std::norm(s[h * 100]);
            const double thd = std::sqrt((odd + even) / std::norm(s[100]));
            if (c != 4) CHECK(thd + 1e-7 >= previous);
            previous = thd;
            CHECK(10 * std::log10(std::max(1e-30, even / std::norm(s[100]))) < -100);
        }
        auto p = instance(c, 12, 2, 50);
        const auto s = spectrum(sine(p, 1000, std::sqrt(2.) * std::pow(10., -.9), 48000, 4800));
        CHECK(20 * std::log10(std::abs(s[200] / s[100])) > -40);
        p.reset();
        std::vector<double> dc(96000, .2);
        process(p, dc);
        double mean = 0;
        for (size_t n = 48000; n < dc.size(); ++n) mean += dc[n];
        CHECK(std::abs(mean / 48000) < 1e-5);
    }
}
TEST_CASE("drive 5 kHz aliases at 24 dB: smooth 4x and hard-fold 8x") {
    // -18 dBFS RMS, -14.9897 dBFS peak; coherent 0.1 s observation after 1 s settle.
    for (int c = 0; c < 6; ++c)
        for (int q : {2, 3}) {
            auto p = instance(c, 24, q);
            auto s = spectrum(sine(p, 5000, std::sqrt(2.) * std::pow(10., -.9), 48000, 4800));
            size_t worstBin = 0;
            double worst = 0;
            for (size_t k = 1; k < s.size(); ++k)
                if (k != 500 && k != 1500)
                    if (std::abs(s[k]) > worst) {
                        worst = std::abs(s[k]);
                        worstBin = k;
                    }
            const double alias = 20 * std::log10(std::max(1e-30, worst / std::abs(s[500])));
            std::cout << "alias " << kCurveChoices[c].key << " " << (1 << q) << "x " << alias
                      << " dBc, bin " << worstBin << "\n";
            if (c < 4 && q == 2) CHECK(alias <= -90);
            if (c >= 4 && q == 3) CHECK(alias <= -60);
        }
}
TEST_CASE("drive reference RMS compensation: pink noise loudness and level dependence") {
    auto input = pink();
    for (int c = 0; c < 6; ++c) {
        double worstReference = 0, lowMin = 0, lowMax = 0, highMin = 0, highMax = 0;
        for (double level : {-30., -18., -6.})
            for (double d : {0., 6., 12., 18., 24., 30., 36.}) {
                auto p = instance(c, d);
                p.setParam(kAutoGain, 1);
                p.reset();
                std::vector<double> x = input;
                const double scale = std::pow(10., level / 20);
                for (auto& y : x) y *= scale;
                const double before = loudness(x);
                process(p, x);
                // Compare matching samples after fixed FIR startup, not a truncated tail.
                std::vector<double> aligned(x.begin() + p.latency() + 1024, x.end());
                std::vector<double> reference(input.begin() + 1024, input.end() - p.latency());
                for (auto& y : reference) y *= scale;
                const double delta = loudness(aligned) - loudness(reference);
                CHECK(std::isfinite(before));
                if (level == -18) {
                    worstReference = std::max(worstReference, std::abs(delta));
                    CHECK(std::abs(delta) <= 1);
                }
                if (level == -30) {
                    lowMin = std::min(lowMin, delta);
                    lowMax = std::max(lowMax, delta);
                }
                if (level == -6) {
                    highMin = std::min(highMin, delta);
                    highMax = std::max(highMax, delta);
                }
            }
        std::cout << "loudness " << kCurveChoices[c].key << " -18 max=" << worstReference
                  << " LU; -30=" << lowMin << ".." << lowMax << "; -6=" << highMin << ".."
                  << highMax << " LU\n";
    }
}
TEST_CASE("drive automation block invariance and restored state") {
    auto render = [](int block) {
        auto p = instance(0, 6);
        std::vector<double> x(8192);
        for (size_t i = 0; i < x.size(); ++i) x[i] = .2 * std::sin(.13 * i);
        size_t pos = 0;
        while (pos < x.size()) {
            if (pos % 173 == 0) {
                p.setParam(kDrive, (pos / 173) % 7 * 6);
                p.setParam(kCurve, (pos / 173) % 6);
                p.setParam(kBias, pos % 346 ? 30 : -30);
                p.setParam(kTone, pos % 346 ? 6 : -6);
                p.setParam(kMix, pos % 346 ? 50 : 100);
            }
            const auto count =
                std::min({static_cast<size_t>(block), (pos / 173 + 1) * 173 - pos, x.size() - pos});
            process(p, x, pos, count);
            pos += count;
        }
        return x;
    };
    const auto one = render(1);
    for (int block : {7, 64, 4096}) CHECK(render(block) == one);
    auto patch = patchType().defaults();
    std::mt19937 rng(8);
    for (size_t i = 0; i < 8; ++i)
        patch.body.values[i] = params::clampPhysical(
            kParams[i], kParams[i].min + (kParams[i].max - kParams[i].min) *
                                             static_cast<double>(rng()) / rng.max());
    auto loaded = patchType().parse(patchType().serialize(patch));
    REQUIRE(loaded);
    Processor p, q;
    for (size_t i = 0; i < 8; ++i) {
        p.setParam(i, patch.body.values[i]);
        q.setParam(i, loaded->body.values[i]);
    }
    p.prepare(48000, 4096, 1);
    q.prepare(48000, 4096, 1);
    std::vector<double> a(4096, .1), b = a;
    process(p, a);
    process(q, b);
    CHECK(a == b);
}
TEST_CASE("drive five-second full-range automation is finite, bounded, mono and stereo") {
    std::mt19937 rng(123);
    Processor p;
    p.prepare(48000, 64, 2);
    double l[64], r[64];
    double* io[] = {l, r};
    for (int pos = 0; pos < 240000;) {
        const int n = 1 + static_cast<int>(rng() % 64);
        for (size_t i = 0; i < 8; ++i)
            p.setParam(i, kParams[i].min + (kParams[i].max - kParams[i].min) *
                                               static_cast<double>(rng()) / rng.max());
        for (int i = 0; i < n; ++i) {
            l[i] = static_cast<double>(rng()) / rng.max() - .5;
            r[i] = static_cast<double>(rng()) / rng.max() - .5;
        }
        p.process(io, n, true);
        for (int i = 0; i < n; ++i) {
            CHECK(std::isfinite(l[i]));
            CHECK(std::isfinite(r[i]));
            CHECK(std::abs(l[i]) < 100);
            CHECK(std::abs(r[i]) < 100);
        }
        pos += n;
    }
    const auto telemetry = p.takeTelemetry();
    CHECK(std::isfinite(telemetry.inputPeakDb));
    CHECK(std::isfinite(telemetry.outputPeakDb));
    p.reset();
    float f[512]{}, g[512]{};
    float* floats[] = {f, g};
    p.process(floats, 512);
    for (float x : f) CHECK(std::isfinite(x));
}
TEST_CASE("drive tilt pair small-signal response follows ADAA at every sample rate") {
    for (double fs : {44100., 48000., 88200., 96000., 192000.})
        for (double tone : {-6., 0., 6.})
            for (double hz : {100., 1000., 8000.}) {
                auto p = instance(0, 0, 2, 0, fs);
                p.setParam(kTone, tone);
                p.reset();
                const auto x = sine(p, hz, 1e-5, fs, static_cast<size_t>(fs / 10));
                double power = 0;
                for (double y : x) power += y * y;
                const double omega = 2 * pi * hz / (fs * 4),
                             alpha = std::exp(-2 * pi * 5 / (fs * 4));
                const auto z = std::polar(1.0, -omega);
                const double dc = std::abs((1.0 - z) / (1.0 - alpha * z));
                const double expected = std::pow(std::cos(omega * .5), 2) * dc;
                CHECK(std::abs(10 * std::log10(2 * power / x.size() / 1e-10) -
                               20 * std::log10(expected)) < .01);
            }
}

TEST_CASE("drive skill recipes validate and render deterministic golden statistics") {
    std::ifstream file(DRIVE_GUIDE_PATH);
    REQUIRE(file.good());
    const std::string guide((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    size_t offset = 0;
    int recipe = 0;
    while ((offset = guide.find("`{\"params\":", offset)) != std::string::npos) {
        const auto end = guide.find('`', offset + 1);
        REQUIRE(end != std::string::npos);
        auto j = json::parse(guide.substr(offset + 1, end - offset - 1));
        REQUIRE(j);
        auto patch = patchType().merge(patchType().defaults(), *j);
        REQUIRE(patch);
        Processor p;
        for (size_t i = 0; i < 8; ++i) p.setParam(i, patch->body.values[i]);
        p.prepare(48000, 4096, 1);
        std::vector<double> x(8192);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = .2 * std::sin(.13 * i) + .1 * std::cos(.077 * i);
        process(p, x);
        double peak = 0, power = 0;
        for (size_t i = 4096; i < x.size(); ++i) {
            peak = std::max(peak, std::abs(x[i]));
            power += x[i] * x[i];
        }
        std::cout << "recipe " << recipe << " peak=" << peak << " rms=" << std::sqrt(power / 4096)
                  << "\n";
        constexpr double expectedPeak[] = {.284340, .255588, .171353};
        constexpr double expectedRms[] = {.157131, .133200, .108421};
        REQUIRE(recipe < 3);
        CHECK(std::abs(peak - expectedPeak[recipe]) < 2e-6);
        CHECK(std::abs(std::sqrt(power / 4096) - expectedRms[recipe]) < 2e-6);
        CHECK(std::isfinite(power));
        ++recipe;
        offset = end + 1;
    }
    CHECK(recipe == 3);
}
