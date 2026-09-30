#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include "eq/processor.h"
#include "supple/base/rng.h"
#include "supple/dsp/dsp.h"
#include "supple/json/json.h"
using namespace supple;
namespace {
eq::Values flat(int phase = 0) {
    auto v = eq::defaults();
    v[eq::kPhase] = phase;
    for (std::size_t b = 0; b < 8; ++b) v[b * 7] = 0;
    return v;
}
void configureValues(eq::Processor& p, const eq::Values& v) {
    for (std::size_t i = 0; i < v.size(); ++i) p.setParam(i, v[i]);
}
std::vector<double> render(eq::Values v, double fs, int block, int frames, bool events = false) {
    eq::Processor p;
    configureValues(p, v);
    p.prepare(fs, 4096);
    std::vector<double> out(static_cast<std::size_t>(frames)), right(out.size());
    std::vector<double> in(out.size());
    for (int i = 0; i < frames; ++i)
        in[static_cast<std::size_t>(i)] =
            .01 * std::sin(2 * std::numbers::pi * 137 * i / fs) + .005 * std::cos(.029 * i);
    for (int offset = 0; offset < frames;) {
        if (events && offset % 997 == 0) {
            p.setParam(2, 200 + offset % 18000);
            p.setParam(3, offset % 25 - 12);
            p.setParam(1, (offset / 997) % 8);
            p.setParam(0, (offset / 997) % 2);
        }
        int n = std::min(block, frames - offset);
        if (events) n = std::min(n, 997 - offset % 997);
        const double* ins[] = {in.data() + offset, in.data() + offset};
        double* outs[] = {out.data() + offset, right.data() + offset};
        p.process(ins, outs, static_cast<std::uint32_t>(n));
        offset += n;
    }
    return out;
}
double measure(eq::Values v, double hz, double fs) {
    eq::Processor p;
    configureValues(p, v);
    p.prepare(fs, 4096);
    const int n = static_cast<int>(std::max(fs * .15, fs * 15 / hz));
    double xx = 0, yy = 0;
    std::array<double, 256> input{}, output{}, other{};
    for (int offset = 0; offset < 2 * n;) {
        const auto count = std::min(256, 2 * n - offset);
        for (int k = 0; k < count; ++k)
            input[static_cast<std::size_t>(k)] =
                .001 * std::sin(2 * std::numbers::pi * hz * (offset + k) / fs);
        const double* in[] = {input.data(), input.data()};
        double* out[] = {output.data(), other.data()};
        p.process(in, out, static_cast<std::uint32_t>(count));
        for (int k = 0; k < count; ++k)
            if (offset + k >= n) {
                xx += input[static_cast<std::size_t>(k)] * input[static_cast<std::size_t>(k)];
                yy += output[static_cast<std::size_t>(k)] * output[static_cast<std::size_t>(k)];
            }
        offset += count;
    }
    return 10 * std::log10(yy / xx);
}
}  // namespace
TEST_CASE("EQ table, canonical patch round trips, transactional merge errors") {
    REQUIRE(params::validateTable(eq::table()));
    for (std::size_t i = 0; i < eq::kParamCount; ++i) CHECK(eq::table()[i].id == i);
    const auto& type = eq::patchType();
    auto d = type.defaults();
    auto full = d;
    for (std::size_t i = 0; i < eq::kParamCount; ++i) full.body.values[i] = eq::table()[i].max;
    for (const auto* p : {&d, &full}) {
        const auto text = type.serialize(*p);
        const auto parsed = type.parse(text);
        REQUIRE(parsed);
        CHECK(parsed->body == p->body);
        CHECK(type.serialize(*parsed) == text);
    }
    auto bad = json::parse(R"({"params":{"band2.freq":-1}})");
    REQUIRE(bad);
    const auto merged = type.merge(d, *bad);
    REQUIRE_FALSE(merged);
    CHECK_FALSE(merged.error().path.empty());
    CHECK(eq::describe(eq::defaults()).size() <= 300);
    CHECK(eq::describe(flat()).size() <= 300);
}
TEST_CASE("EQ flat/off is bit exact, mono and in-place") {
    for (int phase : {0, 1}) {
        auto v = flat(phase);
        for (std::size_t b = 0; b < 8; ++b) {
            v[b * 7] = 1;
            v[b * 7 + 1] = eq::Bell;
        }
        eq::Processor p;
        configureValues(p, v);
        p.prepare(48000, 4096, 1);
        std::array<double, 64> x{}, y{};
        for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::sin(static_cast<double>(i));
        const double* in[] = {x.data()};
        double* out[] = {y.data()};
        p.process(in, out, 64);
        CHECK(x == y);
        p.process(in, const_cast<double**>(in), 64);
        CHECK(x == y);
        CHECK(p.latency() == 0);
    }
}
TEST_CASE("EQ automation is bit identical across host partitions") {
    for (int phase : {0, 1}) {
        auto v = flat(phase);
        const auto reference = render(v, 48000, 1, 9000, true);
        for (int block : {7, 64, 4096}) CHECK(render(v, 48000, block, 9000, true) == reference);
    }
}
TEST_CASE("EQ bell/shelf/pass response and analytic/process agreement across rates") {
    for (double fs : {44100., 48000., 88200., 96000., 192000.}) {
        auto v = flat();
        v[0] = 1;
        v[1] = eq::Bell;
        v[2] = 1000;
        v[3] = 6;
        v[4] = 1;
        CHECK(eq::magnitudeDb(v, 1000, fs) == doctest::Approx(6).epsilon(.005));
        CHECK(measure(v, 1000, fs) == doctest::Approx(6).epsilon(.005));
        v[4] = .71;
        for (int t : {eq::LowShelf, eq::HighShelf}) {
            v[1] = t;
            const double hz = t == eq::LowShelf ? 100 : 10000;
            CHECK(eq::magnitudeDb(v, hz, fs) == doctest::Approx(6).epsilon(.017));
        }
        for (int slope = 0; slope < 8; ++slope) {
            v[1] = eq::LowPass;
            v[5] = slope;
            CHECK(eq::magnitudeDb(v, 1000, fs) == doctest::Approx(-3.01).epsilon(.033));
        }
        v[1] = eq::Bell;
        v[4] = 1;
        for (int k = 0; k < 20; ++k) {
            const double hz = 40 * std::pow(400., k / 19.);
            CAPTURE(fs);
            CAPTURE(hz);
            CHECK(std::abs(measure(v, hz, fs) - eq::magnitudeDb(v, hz, fs)) < .05);
        }
    }
}
TEST_CASE("EQ natural bell/shelf follows analog prototypes through 20 kHz") {
    auto v = flat(1);
    v[0] = 1;
    v[3] = 6;
    v[4] = .71;
    for (int type : {eq::Bell, eq::LowShelf, eq::HighShelf})
        for (double gain : {-30., 6., 30.})
            for (double q : {.1, .71, 24.})
                for (double freq : {30., 200., 1000., 8000., 16000., 22000.}) {
                    v[3] = gain;
                    v[4] = q;
                    v[1] = type;
                    v[2] = freq;
                    for (int k = 0; k < 103; ++k) {
                        const double a = std::pow(10., v[3] / 40.);
                        const double centre = type == eq::Bell
                                                  ? freq
                                                  : (type == eq::LowShelf ? freq / std::sqrt(a)
                                                                          : freq * std::sqrt(a));
                        const double hz =
                            k < 100 ? 20 * std::pow(1000., k / 99.)
                                    : centre * (1 + (k - 101) * .5 / (q * std::max(a, 1 / a)));
                        if (hz <= 0 || hz > 20000) continue;
                        const double w = hz / freq;
                        const std::complex<double> s(0, w);
                        std::complex<double> h;
                        if (type == eq::Bell)
                            h = 1. + (a * a - 1) * s / (v[4] * a) / (s * s + s / (v[4] * a) + 1.);
                        else if (type == eq::LowShelf) {
                            const auto z = s * std::sqrt(a);
                            h = 1. + ((a - 1) * z / v[4] + a * a - 1.) / (z * z + z / v[4] + 1.);
                        } else {
                            const auto z = s / std::sqrt(a);
                            h = a * a +
                                ((1 - a) * a * z / v[4] + 1. - a * a) / (z * z + z / v[4] + 1.);
                        }
                        CAPTURE(type);
                        CAPTURE(freq);
                        CAPTURE(hz);
                        CHECK(std::abs(eq::magnitudeDb(v, hz, 48000) -
                                       20 * std::log10(std::abs(h))) < .1);
                    }
                }
    v[3] = 6;
    v[4] = .71;
    for (double fs : {44100., 48000., 88200., 96000., 192000.}) {
        v[1] = eq::Bell;
        v[2] = 1000;
        for (int k = 0; k < 20; ++k) {
            const double hz = 40 * std::pow(400., k / 19.);
            CAPTURE(fs);
            CAPTURE(hz);
            CHECK(std::abs(measure(v, hz, fs) - eq::magnitudeDb(v, hz, fs)) < .05);
        }
    }
}
TEST_CASE("EQ channel placement preserves side silence and untouched channels") {
    for (int phase : {0, 1})
        for (int placement : {eq::Left, eq::Right, eq::Mid, eq::Side}) {
            auto v = flat(phase);
            v[0] = 1;
            v[2] = 1000;
            v[3] = 6;
            v[6] = placement;
            eq::Processor p;
            configureValues(p, v);
            p.prepare(48000, 64);
            std::array<double, 64> x{}, l{}, r{};
            for (std::size_t i = 0; i < x.size(); ++i)
                x[i] = .1 * std::sin(static_cast<double>(i) * .13);
            const double* in[] = {x.data(), x.data()};
            double* out[] = {l.data(), r.data()};
            p.process(in, out, 64);
            if (placement == eq::Mid) CHECK(l == r);
            if (placement == eq::Side) CHECK(l == x);
            if (placement == eq::Left) CHECK(r == x);
            if (placement == eq::Right) CHECK(l == x);
        }
}
TEST_CASE("EQ linear impulse symmetry, latency, partition invariance and kernel updates") {
    auto v = flat(2);
    v[0] = 1;
    v[2] = 1000;
    v[3] = 6;
    for (double fs : {44100., 48000., 96000.}) {
        eq::Processor p;
        configureValues(p, v);
        p.prepare(fs, 4096);
        const int delay = p.latency();
        CHECK(delay == std::lround(2048 * fs / 48000));
        std::vector<double> x(static_cast<std::size_t>(delay * 2 + 512)), y(x.size()), z(x.size());
        x[0] = 1;
        const double* in[] = {x.data(), x.data()};
        double* out[] = {y.data(), z.data()};
        p.process(in, out, static_cast<uint32_t>(x.size()));
        for (int k = 1; k < delay - 128; ++k)
            CHECK(y[static_cast<std::size_t>(delay - k)] ==
                  doctest::Approx(y[static_cast<std::size_t>(delay + k)]).epsilon(1e-8));
    }
    const auto ref = render(v, 48000, 1, 10000);
    for (int block : {7, 64, 4096}) CHECK(render(v, 48000, block, 10000) == ref);
    eq::Processor p;
    configureValues(p, v);
    p.prepare(48000, 4096);
    v[3] = -6;
    p.setLinearValues(v);
    std::array<double, 4096> x{}, y{}, z{};
    x.fill(.01);
    const double* in[] = {x.data(), x.data()};
    double* out[] = {y.data(), z.data()};
    p.process(in, out, 4096);
    for (auto sample : y) CHECK(std::isfinite(sample));
    p.collect();
}
TEST_CASE("EQ full-range automation stress for five seconds and silent tails") {
    // An EQ intentionally permits cumulative boosts above +40 dB. Low-level
    // excitation tests the full domain without imposing a hidden output limiter.
    for (int phase : {0, 1, 2}) {
        eq::Processor p;
        auto v = flat(phase);
        configureValues(p, v);
        p.prepare(22050, 64);
        Rng rng(17);
        double max = 0;
        int nextKernel = 0;
        for (int offset = 0; offset < 5 * 22050;) {
            const int n = 1 + static_cast<int>(rng.next() % 64);
            std::array<double, 64> x{}, y{}, z{};
            for (std::size_t i = 0; i < eq::kPhase; ++i) {
                const auto& s = eq::table()[i];
                v[i] = params::fromNormalized(s, rng.uniform(0., 1.));
                p.setParam(i, v[i]);
            }
            v[eq::kOutput] = rng.uniform(-24., 24.);
            v[eq::kAutoGain] = rng.next() % 2;
            p.setParam(eq::kOutput, v[eq::kOutput]);
            p.setParam(eq::kAutoGain, v[eq::kAutoGain]);
            if (phase == 2 && offset >= nextKernel) {
                // Model the control worker's coalescing on a deterministic sample grid.
                p.setLinearValues(v);
                p.collect();
                nextKernel += 4096;
            }
            for (int k = 0; k < n; ++k) x[static_cast<std::size_t>(k)] = rng.uniform(-1e-24, 1e-24);
            const double* in[] = {x.data(), x.data()};
            double* out[] = {y.data(), z.data()};
            p.process(in, out, static_cast<uint32_t>(n));
            for (int k = 0; k < n; ++k) {
                REQUIRE(std::isfinite(y[static_cast<std::size_t>(k)]));
                REQUIRE(std::isfinite(z[static_cast<std::size_t>(k)]));
                max = std::max({max, std::abs(y[static_cast<std::size_t>(k)]),
                                std::abs(z[static_cast<std::size_t>(k)])});
            }
            offset += n;
        }
        std::array<double, 64> silence{}, left{}, right{};
        const double* input[] = {silence.data(), silence.data()};
        double* output[] = {left.data(), right.data()};
        for (int tail = 0; tail < 2 * 22050; tail += 64) {
            p.process(input, output, 64);
            for (std::size_t i = 0; i < left.size(); ++i) {
                REQUIRE(std::isfinite(left[i]));
                REQUIRE(std::isfinite(right[i]));
                max = std::max({max, std::abs(left[i]), std::abs(right[i])});
            }
        }
        CHECK(max < 100);
    }
}
TEST_CASE("EQ save/load into a fresh engine reproduces output") {
    auto patch = eq::patchType().defaults();
    Rng rng(9);
    for (std::size_t i = 0; i < eq::kPhase; ++i)
        patch.body.values[i] = params::fromNormalized(eq::table()[i], rng.uniform(0., 1.));
    patch.body.values[eq::kAutoGain] = 1;
    patch.body.values[eq::kOutput] = rng.uniform(-24., 24.);
    const auto parsed = eq::patchType().parse(eq::patchType().serialize(patch));
    REQUIRE(parsed);
    eq::Values a{}, b{};
    std::copy(patch.body.values.begin(), patch.body.values.end(), a.begin());
    std::copy(parsed->body.values.begin(), parsed->body.values.end(), b.begin());
    CHECK(render(a, 48000, 64, 5000) == render(b, 48000, 7, 5000));
}
TEST_CASE("EQ pass stopband slopes and bell bandwidth follow their prototypes") {
    auto v = flat();
    v[0] = 1;
    v[1] = eq::HighPass;
    v[2] = 1000;
    v[4] = .71;
    constexpr int slopes[] = {6, 12, 18, 24, 36, 48, 72, 96};
    for (int i = 0; i < 8; ++i) {
        v[5] = i;
        const double octave = eq::magnitudeDb(v, 250, 48000) - eq::magnitudeDb(v, 125, 48000);
        CHECK(std::abs(octave - slopes[i]) < 1.);
    }
    v[1] = eq::Bell;
    v[3] = 6;
    v[4] = 1;
    const double bw = 1;
    const double lo = (std::sqrt(bw * bw + 4) - bw) / 2, hi = 1 / lo;
    const double digitalLow =
        48000 / std::numbers::pi * std::atan(lo * std::tan(std::numbers::pi / 48.));
    const double digitalHigh =
        48000 / std::numbers::pi * std::atan(hi * std::tan(std::numbers::pi / 48.));
    CHECK(std::abs(digitalLow / (lo * 1000) - 1) < .05);
    CHECK(std::abs(digitalHigh / (hi * 1000) - 1) < .05);
    CHECK(std::abs(eq::magnitudeDb(v, digitalLow, 48000) - 3) < .05);
    CHECK(std::abs(eq::magnitudeDb(v, digitalHigh, 48000) - 3) < .05);
}
TEST_CASE(
    "EQ linear finite response matches a measured sinusoid, and reprepare changes rate safely") {
    auto v = flat(2);
    v[0] = 1;
    v[1] = eq::Bell;
    v[2] = 1000;
    v[3] = 6;
    for (double fs : {44100., 48000., 88200., 96000., 192000.})
        for (double hz : {100., 1000., 8000., 16000.}) {
            CAPTURE(fs);
            CAPTURE(hz);
            CHECK(std::abs(measure(v, hz, fs) - eq::magnitudeDb(v, hz, fs)) < .05);
        }
    eq::Processor p;
    configureValues(p, v);
    for (double fs : {192000., 22050., 384000., 48000.}) {
        p.prepare(fs, 64);
        p.reset();
        std::array<double, 64> x{}, y{}, z{};
        const double* in[] = {x.data(), x.data()};
        double* out[] = {y.data(), z.data()};
        p.process(in, out, 64);
        CHECK(y == x);
    }
}
TEST_CASE("EQ auto gain is a static compensation and output ramps suppress step spectrum") {
    auto v = flat();
    v[0] = 1;
    v[1] = eq::Bell;
    v[2] = 1000;
    v[3] = 6;
    v[eq::kAutoGain] = 1;
    CHECK(std::abs(measure(v, 1000, 48000) - eq::magnitudeDb(v, 1000, 48000)) < .05);
    eq::Processor p;
    configureValues(p, flat());
    p.prepare(48000, 4096);
    constexpr std::size_t n = 4096;
    std::array<double, n> in{}, smooth{}, right{}, abrupt{};
    for (std::size_t i = 0; i < n; ++i) {
        in[i] = .1 * std::sin(2 * std::numbers::pi * 93 * static_cast<double>(i) / n);
        abrupt[i] = in[i] * (i >= 1024 ? std::pow(10., 24 / 20.) : 1.);
    }
    const double* input[] = {in.data(), in.data()};
    double* out[] = {smooth.data(), right.data()};
    p.process(input, out, 1024);
    p.setParam(eq::kOutput, 24);
    const double* rest[] = {in.data() + 1024, in.data() + 1024};
    double* tail[] = {smooth.data() + 1024, right.data() + 1024};
    p.process(rest, tail, 3072);
    // Hann-windowed high-frequency energy measures the zipper step, away from the carrier.
    for (std::size_t i = 0; i < n; ++i) {
        const double w =
            .5 - .5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / (n - 1));
        smooth[i] *= w;
        abrupt[i] *= w;
    }
    std::array<std::complex<double>, n / 2 + 1> a{}, b{};
    supple::dsp::Fft fft;
    fft.prepare(n);
    REQUIRE(fft.forward(smooth, a));
    REQUIRE(fft.forward(abrupt, b));
    double quiet = 0, loud = 0;
    for (std::size_t i = 400; i < a.size(); ++i) {
        quiet += std::norm(a[i]);
        loud += std::norm(b[i]);
    }
    CHECK(quiet < loud * .01);
}

TEST_CASE("EQ auto gain stays bounded when eight steep passes remove most of the spectrum") {
    auto v = flat();
    for (std::size_t b = 0; b < eq::kBands; ++b) {
        v[eq::bandParam(b, eq::Enabled)] = 1;
        v[eq::bandParam(b, eq::Type)] = eq::HighPass;
        v[eq::bandParam(b, eq::Freq)] = 22000;
        v[eq::bandParam(b, eq::Slope)] = 7;
    }
    v[eq::kAutoGain] = 1;
    CHECK(eq::compensationDb(v, 48000) == 24);
    eq::Processor p;
    configureValues(p, v);
    p.prepare(48000, 4096);
    std::array<float, 4096> x{}, y{}, z{};
    x[0] = .1f;
    const float* input[] = {x.data(), x.data()};
    float* output[] = {y.data(), z.data()};
    p.process(input, output, 4096);
    for (auto sample : y) CHECK(std::isfinite(sample));
}
TEST_CASE("EQ natural phase ignores controls outside each filter's design") {
    auto v = flat(1);
    v[0] = 1;
    v[2] = 20000;
    v[1] = eq::HighPass;
    v[5] = 3;
    const auto pass = render(v, 48000, 64, 4096);
    v[3] = 30;
    v[4] = 24;
    CHECK(render(v, 48000, 7, 4096) == pass);
    v[1] = eq::Tilt;
    v[3] = 6;
    v[4] = .1;
    const auto tilt = render(v, 48000, 64, 4096);
    v[4] = 24;
    CHECK(render(v, 48000, 7, 4096) == tilt);
}
