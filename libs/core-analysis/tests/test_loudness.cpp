// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include "supple/analysis/loudness.h"
#include "supple/dsp/dsp.h"

using namespace supple::analysis;
namespace {
constexpr double pi = std::numbers::pi;
struct Segment {
    double seconds, db;
};
struct Result {
    double i, m, s, lra, tp;
};
void check(double actual, double expected, double tolerance) {
    CHECK(std::abs(actual - expected) <= tolerance);
}
Result measure(double rate, Layout layout, const std::vector<Segment>& segments,
               bool alternating = false) {
    int channelCount = layout == Layout::Surround50 ? 5 : 2;
    double seconds = 0;
    for (auto segment : segments) seconds += segment.seconds;
    LoudnessMeter exact, hist;
    exact.prepare(rate, layout, LoudnessMeter::Mode::Exact, seconds + 1);
    hist.prepare(rate, layout, LoudnessMeter::Mode::Histogram);
    std::array<std::vector<float>, 5> audio;
    for (auto& channel : audio) channel.resize(4800);
    std::array<const float*, 5> ptr{};
    for (int c = 0; c < channelCount; ++c)
        ptr[static_cast<std::size_t>(c)] = audio[static_cast<std::size_t>(c)].data();
    std::int64_t position = 0;
    for (auto segment : segments) {
        const auto total = static_cast<std::int64_t>(std::llround(segment.seconds * rate));
        for (std::int64_t done = 0; done < total;) {
            const int count = static_cast<int>(std::min<std::int64_t>(4800, total - done));
            for (int j = 0; j < count; ++j) {
                const double sine = std::sin(2 * pi * 1000 * (position + j) / rate);
                for (int c = 0; c < channelCount; ++c) {
                    double db = segment.db;
                    if (layout == Layout::Surround50) db = c == 2 ? -24 : (c >= 3 ? -30 : -28);
                    if (alternating && c == 1) db = segment.db;
                    audio[static_cast<std::size_t>(c)][static_cast<std::size_t>(j)] =
                        static_cast<float>(std::pow(10.0, db / 20) * sine);
                }
            }
            exact.process(
                std::span<const float* const>(ptr.data(), static_cast<std::size_t>(channelCount)),
                count);
            hist.process(
                std::span<const float* const>(ptr.data(), static_cast<std::size_t>(channelCount)),
                count);
            position += count;
            done += count;
        }
    }
    const Result a{exact.integrated(), exact.maxMomentary(), exact.maxShortTerm(),
                   exact.loudnessRange(), exact.truePeak()};
    const Result b{hist.integrated(), hist.maxMomentary(), hist.maxShortTerm(),
                   hist.loudnessRange(), hist.truePeak()};
    for (auto [x, y] : {std::pair{a.i, b.i}, {a.m, b.m}, {a.s, b.s}, {a.lra, b.lra}, {a.tp, b.tp}})
        if (std::isfinite(x)) {
            if (std::abs(x - y) > 0.02)
                std::cout << "mode mismatch exact=" << x << " hist=" << y << "\n";
            check(x, y, 0.02);
        }
    return a;
}
void print(const std::string& name, double rate, double value, double expected) {
    std::cout << "EBU " << name << " @" << rate << " measured=" << value << " expected=" << expected
              << '\n';
}
std::vector<Segment> repeat(Segment a, Segment b, int times) {
    std::vector<Segment> v;
    for (int i = 0; i < times; ++i) {
        v.push_back(a);
        v.push_back(b);
    }
    return v;
}
}  // namespace

TEST_CASE("Tech 3341 integrated, M and S cases 1-6, 9, 10, 12, 13") {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        auto c1 = measure(rate, Layout::Stereo, {{20, -23}});
        print("1 I", rate, c1.i, -23);
        check(c1.i, -23, 0.1);
        check(c1.m, -23, 0.1);
        check(c1.s, -23, 0.1);
        auto c3 = measure(rate, Layout::Stereo, {{10, -36}, {60, -23}, {10, -36}});
        print("3 I", rate, c3.i, -23);
        check(c3.i, -23, 0.1);
        if (rate != 48000) continue;
        auto c2 = measure(rate, Layout::Stereo, {{20, -33}});
        print("2 I", rate, c2.i, -33);
        check(c2.i, -33, 0.1);
        check(c2.m, -33, 0.1);
        check(c2.s, -33, 0.1);
        auto c4 =
            measure(rate, Layout::Stereo, {{10, -72}, {10, -36}, {60, -23}, {10, -36}, {10, -72}});
        print("4 I", rate, c4.i, -23);
        check(c4.i, -23, 0.1);
        auto c5 = measure(rate, Layout::Stereo, {{20, -26}, {20.1, -20}, {20, -26}});
        print("5 I", rate, c5.i, -23);
        check(c5.i, -23, 0.1);
        auto c6 = measure(rate, Layout::Surround50, {{20, -23}});
        print("6 I", rate, c6.i, -23);
        check(c6.i, -23, 0.1);
        auto c9 = measure(rate, Layout::Stereo, repeat({1.34, -20}, {1.66, -30}, 5));
        print("9 S", rate, c9.s, -23);
        check(c9.s, -23, 0.1);
        auto c12 = measure(rate, Layout::Stereo, repeat({0.18, -20}, {0.22, -30}, 25));
        print("12 M", rate, c12.m, -23);
        check(c12.m, -23, 0.1);
        for (int i = 0; i < 20; ++i) {
            auto c10 = measure(rate, Layout::Stereo, {{i * .15, -200}, {3, -23}, {1, -200}});
            print("10/11 S " + std::to_string(i), rate, c10.s, -23);
            check(c10.s, -23, 0.1);
            auto c13 = measure(rate, Layout::Stereo, {{i * .02, -200}, {.4, -23}, {1, -200}});
            print("13/14 M " + std::to_string(i), rate, c13.m, -23);
            check(c13.m, -23, 0.1);
        }
    }
}
TEST_CASE("Tech 3342 LRA cases 1-4") {
    const std::array<std::vector<Segment>, 4> cases = {
        std::vector<Segment>{{20, -20}, {20, -30}}, std::vector<Segment>{{20, -20}, {20, -15}},
        std::vector<Segment>{{20, -40}, {20, -20}},
        std::vector<Segment>{{20, -50}, {20, -35}, {20, -20}, {20, -35}, {20, -50}}};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto r = measure(48000, Layout::Stereo, cases[i]);
        const double expected[] = {10, 5, 20, 15};
        print("LRA " + std::to_string(i + 1), 48000, r.lra, expected[i]);
        check(r.lra, expected[i], 1);
    }
}
TEST_CASE("Tech 3341 true peak cases 15-19") {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        for (int c = 15; c <= 19; ++c) {
            const double divisor = c <= 16 || c == 19 ? 4 : (c == 17 ? 6 : 8);
            const double phase = (c == 15              ? 0
                                  : c == 16 || c == 19 ? 45
                                  : c == 17            ? 60
                                                       : 67.5) *
                                 pi / 180;
            const double amplitude = c == 19 ? 1.41 : .5;
            LoudnessMeter meter, exact;
            meter.prepare(rate, Layout::Mono, LoudnessMeter::Mode::Histogram);
            exact.prepare(rate, Layout::Mono, LoudnessMeter::Mode::Exact, 2);
            std::vector<float> block(4096);
            const float* ptr[] = {block.data()};
            const int count = static_cast<int>(rate);
            for (int start = 0; start < count; start += 4096) {
                const int n = std::min(4096, count - start);
                for (int j = 0; j < n; ++j) {
                    const int index = start + j;
                    const double fade =
                        std::min({1.0, index / (.01 * rate), (count - 1 - index) / (.01 * rate)});
                    block[static_cast<std::size_t>(j)] = static_cast<float>(
                        amplitude * fade * std::sin(2 * pi * index / divisor + phase));
                }
                meter.process(ptr, n);
                exact.process(ptr, n);
            }
            check(exact.truePeak(), meter.truePeak(), 1e-12);
            const double expected = c == 19 ? 3 : -6;
            print(std::to_string(c) + " TP", rate, meter.truePeak(), expected);
            CHECK(meter.truePeak() >= expected - .4);
            CHECK(meter.truePeak() <= expected + .2);
        }
    }
}
TEST_CASE("silence remains undefined and K response follows analog prototype") {
    LoudnessMeter meter;
    meter.prepare(48000, Layout::Mono, LoudnessMeter::Mode::Histogram);
    std::vector<float> zero(4800);
    const float* ptr[] = {zero.data()};
    meter.process(ptr, 4800);
    CHECK(std::isinf(meter.integrated()));
    CHECK(std::isinf(meter.truePeak()));
    auto shelf = supple::dsp::kShelf(48000);
    auto hp = supple::dsp::kHighpass(48000);
    for (int k = 0; k < 30; ++k) {
        const double f = 20 * std::pow(1000.0, k / 29.0);
        const auto z = std::polar(1.0, -2 * pi * f / 48000);
        const auto response = [&](supple::dsp::Coefficients c) {
            return (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
        };
        const double measured = 20 * std::log10(std::abs(response(shelf) * response(hp)));
        const auto jw = std::complex<double>(0, 2 * pi * f);
        const double ws = 2 * pi * 1681.974450955533;
        const double wh = 2 * pi * 38.13547087602444;
        const double vh = std::pow(10.0, 3.999843853973347 / 20);
        const double vb = std::pow(vh, 0.4996667741545416);
        const auto analogShelf = (vh * jw * jw + vb * ws / 0.7071752369554196 * jw + ws * ws) /
                                 (jw * jw + ws / 0.7071752369554196 * jw + ws * ws);
        const auto analogHp = jw * jw / (jw * jw + wh / 0.5003270373238773 * jw + wh * wh);
        const double expected = 20 * std::log10(std::abs(analogShelf * analogHp));
        CHECK(std::abs(measured - expected) < 0.01);
    }
}
TEST_CASE("Tech 3341 burst true peak cases 20-23") {
    constexpr double rate = 48000;
    constexpr int up = 4;
    constexpr int total = 48000;
    constexpr int start = 12000;
    // Build a phase-continuous carrier with one inserted high-frequency period
    // on the 4x grid, then low-pass and decimate at four different offsets.
    std::vector<float> high(static_cast<std::size_t>(total * up));
    const int burst = 4 * up;
    for (int i = 0; i < total * up; ++i) {
        const double phase = i < start * up           ? 2 * pi * i / (6 * up)
                             : i < start * up + burst ? 2 * pi * (i - start * up) / burst
                                                      : 2 * pi * (i - burst) / (6 * up);
        high[static_cast<std::size_t>(i)] = static_cast<float>(
            (i >= start * up && i < start * up + burst ? 1.0 : .5) * std::sin(phase));
    }
    for (int offset = 0; offset < 4; ++offset) {
        auto decimated = supple::dsp::resample(
            std::span<const float>(high.data() + offset,
                                   high.size() - static_cast<std::size_t>(offset)),
            rate * up, rate);
        LoudnessMeter meter, exact;
        meter.prepare(rate, Layout::Mono, LoudnessMeter::Mode::Histogram);
        exact.prepare(rate, Layout::Mono, LoudnessMeter::Mode::Exact, 2);
        const float* ptr[] = {decimated.data()};
        meter.process(ptr, static_cast<int>(decimated.size()));
        exact.process(ptr, static_cast<int>(decimated.size()));
        check(exact.truePeak(), meter.truePeak(), 1e-12);
        print(std::to_string(20 + offset) + " TP", rate, meter.truePeak(), 0);
        CHECK(meter.truePeak() >= -.4);
        CHECK(meter.truePeak() <= .2);
    }
}

TEST_CASE("Tech 3341 live-meter successive maxima cases 11 and 14") {
    for (auto mode : {LoudnessMeter::Mode::Exact, LoudnessMeter::Mode::Histogram}) {
        for (bool shortWindow : {false, true}) {
            LoudnessMeter meter;
            meter.prepare(48000, Layout::Stereo, mode, shortWindow ? 121 : 17);
            std::array<float, 4800> samples{};
            const float* ptr[] = {samples.data(), samples.data()};
            std::int64_t absolute = 0;
            auto silence = [&](int frames) {
                samples.fill(0);
                while (frames > 0) {
                    const int n = std::min(frames, 4800);
                    meter.process(ptr, n);
                    absolute += n;
                    frames -= n;
                }
            };
            for (int index = 0; index < 20; ++index) {
                const double db = -38.0 + index;
                const int toneFrames = shortWindow ? 144000 : 19200;
                const int preFrames = shortWindow ? index * 7200 : index * 960;
                const int postFrames = (shortWindow ? 144000 : 19200) - preFrames;
                silence(preFrames);
                for (int start = 0; start < toneFrames; start += 4800) {
                    for (int j = 0; j < 4800; ++j)
                        samples[static_cast<std::size_t>(j)] =
                            static_cast<float>(std::pow(10.0, db / 20) *
                                               std::sin(2 * pi * 1000 * (absolute + j) / 48000));
                    meter.process(ptr, 4800);
                    absolute += 4800;
                }
                print(shortWindow ? "11 S" : "14 M", 48000,
                      shortWindow ? meter.maxShortTerm() : meter.maxMomentary(), db);
                check(shortWindow ? meter.maxShortTerm() : meter.maxMomentary(), db, 0.1);
                silence(postFrames);
            }
        }
    }
}
TEST_CASE("Tech 3341 periodic cases 9 and 12 remain constant") {
    for (auto mode : {LoudnessMeter::Mode::Exact, LoudnessMeter::Mode::Histogram}) {
        for (bool shortWindow : {false, true}) {
            LoudnessMeter meter;
            meter.prepare(48000, Layout::Stereo, mode, 16);
            std::array<float, 4800> samples{};
            const float* ptr[] = {samples.data(), samples.data()};
            const int blocks = shortWindow ? 150 : 100;
            for (int block = 0; block < blocks; ++block) {
                for (int j = 0; j < 4800; ++j) {
                    const std::int64_t sample = static_cast<std::int64_t>(block) * 4800 + j;
                    const int period = shortWindow ? 144000 : 19200;
                    const int high = shortWindow ? 64320 : 8640;
                    const double db = sample % period < high ? -20 : -30;
                    samples[static_cast<std::size_t>(j)] = static_cast<float>(
                        std::pow(10.0, db / 20) * std::sin(2 * pi * 1000 * sample / 48000));
                }
                meter.process(ptr, 4800);
                if (block >= (shortWindow ? 30 : 10))
                    check(shortWindow ? meter.shortTerm() : meter.momentary(), -23, 0.1);
            }
        }
    }
}
TEST_CASE("above 96 kHz true peak uses sample peak") {
    LoudnessMeter meter;
    meter.prepare(192000, Layout::Mono, LoudnessMeter::Mode::Histogram);
    const float values[] = {0, .25f, -.75f, 0};
    const float* ptr[] = {values};
    meter.process(ptr, 4);
    check(meter.truePeak(), 20 * std::log10(.75), 1e-12);
    check(meter.samplePeak(), meter.truePeak(), 1e-12);
}

TEST_CASE("5.1 loudness excludes LFE and weights both rear channels") {
    std::array<std::vector<float>, 6> audio;
    for (auto& channel : audio) channel.resize(48000);
    std::array<const float*, 6> input;
    for (std::size_t c = 0; c < input.size(); ++c) input[c] = audio[c].data();
    double reference = 0;
    for (auto mode : {LoudnessMeter::Mode::Exact, LoudnessMeter::Mode::Histogram}) {
        for (std::size_t active = 0; active < audio.size(); ++active) {
            for (auto& channel : audio) std::fill(channel.begin(), channel.end(), 0);
            for (std::size_t i = 0; i < audio[active].size(); ++i)
                audio[active][i] = static_cast<float>(0.1 * std::sin(2 * pi * 1000 * i / 48000));
            LoudnessMeter meter;
            meter.prepare(48000, Layout::Surround51, mode, 2);
            meter.process(input, 48000);
            if (active == 0) reference = meter.integrated();
            if (active == 3) {
                CHECK(std::isinf(meter.integrated()));
                CHECK(std::isfinite(meter.truePeak()));
            } else {
                CHECK(meter.integrated() ==
                      doctest::Approx(reference + (active >= 4 ? 10 * std::log10(1.41) : 0))
                          .epsilon(0.001));
            }
        }
    }
}
TEST_CASE("unprepared loudness meter safely ignores processing") {
    LoudnessMeter meter;
    const float sample = 0;
    const float* input[] = {&sample, &sample};
    meter.process(input, 1);
    CHECK(std::isinf(meter.integrated()));
}
