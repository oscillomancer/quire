// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own claims, measured through the Engine the plugin runs.
#include <algorithm>
#include <cmath>
#include <memory>

#include "Check.h"
#include "Params.h"
#include "Processor.h"
#include "Testing.h"

using namespace supple;
using namespace product;

namespace {

constexpr double kRate = 48000.0;

}  // namespace

TEST(every_shape_stays_inside_full_scale_and_the_fold_is_the_input_inside_it) {
    for (int shape = 0; shape < 3; ++shape) {
        double previous = Processor::shaped(shape, -100.0);
        for (double x = -100.0; x <= 100.0; x += 0.001) {
            const double y = Processor::shaped(shape, x);
            CHECK(std::fabs(y) <= 1.0);
            CHECK(std::fabs(y - previous) < 0.01);  // continuous: no jumps to click
            previous = y;
        }
    }
    for (double x = -1.0; x <= 1.0; x += 0.01) CHECK_NEAR(Processor::shaped(2, x), x, 1e-12);
    CHECK_NEAR(Processor::shaped(2, 1.5), 0.5, 1e-12);
    CHECK_NEAR(Processor::shaped(2, -2.5), 0.5, 1e-12);
}

TEST(no_mix_is_the_dry_input_exactly_delayed_by_the_reported_latency) {
    for (int quality = 0; quality < 4; ++quality) {
        test::RenderOptions o;
        o.initial = {{kMix, 0.0}, {kOutput, 0.0}, {kDrive, 36.0}, {kQuality, quality}};
        auto engine = std::make_unique<Engine>();
        const test::Signal in = test::noise(2, 9600);
        const test::Signal out = test::render(in, o, engine.get());
        const int latency = engine->latency();
        CHECK(latency == (quality == 0 ? 0 : latency) && (quality == 0) == (latency == 0));
        double worst = 0.0;
        for (size_t c = 0; c < 2; ++c)
            for (size_t n = static_cast<size_t>(latency); n < in[c].size(); ++n)
                worst = std::max(worst, std::fabs(out[c][n] - in[c][n - static_cast<size_t>(latency)]));
        CHECK_MSG(worst == 0.0, "quality %d, latency %d: %g", quality, latency, worst);
    }
}

TEST(oversampling_keeps_what_the_shaper_adds_from_folding_back) {
    // A 5 kHz tone, clipped hard: its 7th and 9th harmonics (35 and 45 kHz)
    // fold to 13 and 3 kHz at 48 kHz, where nothing harmonic is. (Above 0.45
    // of the rate -- 21.6 kHz -- the oversampler's filters are still falling
    // and some folding is allowed: inaudible, and not measured here.)
    auto aliasing = [](int quality) {
        test::RenderOptions o;
        o.initial = {{kShape, 1}, {kDrive, 12.0}, {kQuality, quality}, {kTone, 20000.0}, {kOutput, 0.0}};
        const test::Signal out = test::render(test::sine(2, 48000, 5000.0, kRate, 0.5), o);
        const double fundamental = test::toneAmplitude(out[0], 5000.0, kRate, 24000);
        return std::max(test::toneAmplitude(out[0], 3000.0, kRate, 24000), test::toneAmplitude(out[0], 13000.0, kRate, 24000)) / fundamental;
    };
    const double plain = aliasing(0), eightTimes = aliasing(3);
    std::printf("      worst alias: %.1f dB without oversampling, %.1f dB at 8x\n", 20.0 * std::log10(plain),
                20.0 * std::log10(eightTimes));
    CHECK(eightTimes < plain * 0.1);  // 20 dB less, at least
}

TEST(the_hard_clip_never_passes_the_output_level) {
    test::RenderOptions o;
    // Without oversampling: band-limiting a clipped wave lets it overshoot
    // between samples (which Peak, a true peak, reports).
    o.initial = {{kShape, 1}, {kDrive, 36.0}, {kOutput, 0.0}, {kQuality, 0}};
    const test::Signal out = test::render(test::sine(2, 48000, 220.0, kRate, 0.9), o);
    CHECK(test::peak(out) <= 1.0);
    CHECK(test::peak(out, 24000) > 0.99);  // and it does reach it
}

TEST(more_drive_is_more_third_harmonic) {
    auto thirdOverFirst = [](double drive) {
        test::RenderOptions o;
        o.initial = {{kDrive, drive}};
        const test::Signal out = test::render(test::sine(2, 48000, 1000.0, kRate, 0.25), o);
        return test::toneAmplitude(out[0], 3000.0, kRate, 24000) / test::toneAmplitude(out[0], 1000.0, kRate, 24000);
    };
    const double low = thirdOverFirst(0.0), high = thirdOverFirst(24.0);
    std::printf("      third harmonic: %.1f dB at no drive, %.1f dB at 24 dB\n", 20.0 * std::log10(low),
                20.0 * std::log10(high));
    CHECK(high > low * 10.0);
}

TEST(loudness_reads_a_tone_the_way_broadcast_meters_do) {
    // A stereo 1 kHz tone at -6 dBFS reads -6 LUFS, once three seconds have passed.
    test::RenderOptions o;
    o.initial = {{kMix, 0.0}, {kOutput, 0.0}};
    auto engine = std::make_unique<Engine>();
    test::render(test::sine(2, 4 * 48000, 1000.0, kRate, 0.5), o, engine.get());
    const ParamDef& loudness = *params::find(kLoudness);
    CHECK_NEAR(toPlain(loudness, engine->store().get(static_cast<size_t>(params::indexOf(kLoudness)))), -6.02, 0.1);
}

TEST(peak_reports_the_loudest_point_out) {
    test::RenderOptions o;
    o.initial = {{kMix, 0.0}, {kOutput, 0.0}};
    auto engine = std::make_unique<Engine>();
    test::render(test::sine(2, 9600, 1000.0, kRate, 0.5), o, engine.get());
    const ParamDef& peak = *params::find(kPeak);
    CHECK_NEAR(toPlain(peak, engine->store().get(static_cast<size_t>(params::indexOf(kPeak)))), -6.02, 0.05);
}

SUPPLE_TEST_MAIN
