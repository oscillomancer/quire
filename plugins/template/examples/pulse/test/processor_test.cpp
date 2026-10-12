// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own claims, measured through the Engine the plugin runs.
#include <algorithm>
#include <memory>

#include "Check.h"
#include "Params.h"
#include "Processor.h"
#include "Testing.h"

using namespace supple;
using namespace product;

namespace {

constexpr double kSampleRate = 48000.0;

// A constant input makes the output the gain itself.
test::Signal level(int frames, double value = 0.5) {
    test::Signal s = test::silence(2, frames);
    for (auto& c : s) std::fill(c.begin(), c.end(), value);
    return s;
}

double plainOf(uint32_t id, const char* text) {
    double host = 0.0;
    parseValue(*params::find(id), text, host);
    return toPlain(*params::find(id), host);
}

// Renders with the host's transport playing from `beats`, as a host would
// send it: tempo and song position at the start of each block.
test::Signal playing(const test::Signal& in, std::vector<std::pair<uint32_t, double>> initial, double beats,
                     double tempo, int block, Engine& engine) {
    for (const auto& [id, plain] : initial)
        engine.store().set(static_cast<size_t>(params::indexOf(id)), toHost(*params::find(id), plain));
    engine.activate(kSampleRate, block, 2, 2);
    test::Signal out = in;
    const int frames = static_cast<int>(in[0].size());
    for (int f = 0; f < frames; f += block) {
        Transport t;
        t.playing = t.hasTempo = t.hasBeats = true;
        t.tempo = tempo;
        t.beats = beats + f / kSampleRate * tempo / 60.0;
        double* io[] = {out[0].data() + f, out[1].data() + f};
        NoEvents none;
        engine.process<double>(io, io, std::min(block, frames - f), none, &t);
    }
    return out;
}

}  // namespace

TEST(no_depth_at_unity_output_is_the_input_bit_for_bit) {
    const test::Signal in = test::noise(2, 48000, 3);
    test::RenderOptions o;
    o.initial = {{kDepth, 0.0}, {kSync, 0.0}, {kRate, 7.0}};
    o.blocks = {1, 64, 777};
    CHECK(test::maxDifference(test::render(in, o), in) == 0.0);
}

TEST(free_running_the_gain_is_the_oscillator) {
    for (int shape : {Processor::kSine, Processor::kTriangle, Processor::kSquare}) {
        test::RenderOptions o;
        o.initial = {{kSync, 0.0}, {kRate, 2.0}, {kDepth, 1.0}, {kShape, static_cast<double>(shape)}};
        const test::Signal out = test::render(level(48000), o);
        double error = 0.0;
        for (int i = 0; i < 48000; ++i) {
            const double phase = i * 2.0 / kSampleRate;
            const double want = Processor::shapeAt(shape, phase - std::floor(phase));
            error = std::max(error, std::fabs(out[0][static_cast<size_t>(i)] / 0.5 - want));
        }
        CHECK_MSG(error < 1e-9, "shape %d is off by %g", shape, error);
    }
}

TEST(synced_and_playing_a_pulse_lands_on_the_beat_wherever_playback_starts) {
    // 1/4 at 120 BPM: two cycles a second, each beginning on a beat.
    const std::vector<std::pair<uint32_t, double>> settings = {
        {kSync, 1.0}, {kDivision, plainOf(kDivision, "1/4")}, {kDepth, 1.0}};
    auto a = std::make_unique<Engine>(), b = std::make_unique<Engine>();
    const test::Signal fromOne = playing(level(48000), settings, 1.0, 120.0, 256, *a);
    const test::Signal fromLater = playing(level(36000), settings, 1.5, 120.0, 512, *b);  // 0.25 s later
    // At every beat the oscillator is at its top.
    for (int beat = 0; beat < 2; ++beat) CHECK_NEAR(fromOne[0][static_cast<size_t>(beat * 24000)], 0.5, 1e-9);
    double error = 0.0;
    for (int i = 0; i < 36000; ++i)
        error = std::max(error, std::fabs(fromLater[0][static_cast<size_t>(i)] - fromOne[0][static_cast<size_t>(i + 12000)]));
    CHECK_MSG(error < 1e-9, "starting half a beat later differs by %g", error);
    // A different tempo: 1/4 at 90 BPM is 1.5 cycles a second.
    auto c = std::make_unique<Engine>();
    const test::Signal slower = playing(level(48000), settings, 0.0, 90.0, 300, *c);
    CHECK_NEAR(slower[0][32000], 0.5, 1e-9);  // beat 1 at 90 BPM: 2/3 s, 32000 samples
}

TEST(synced_and_stopped_it_runs_at_the_hosts_tempo_or_120) {
    test::RenderOptions o;
    o.initial = {{kSync, 1.0}, {kDivision, plainOf(kDivision, "1/8")}, {kDepth, 1.0}};
    const test::Signal out = test::render(level(24000), o);  // no transport: 120 BPM, 1/8 is 4 Hz
    for (int cycle = 0; cycle < 2; ++cycle) CHECK_NEAR(out[0][static_cast<size_t>(cycle * 12000)], 0.5, 1e-9);
    CHECK_NEAR(out[0][6000], 0.0, 1e-9);
}

TEST(half_a_cycle_apart_the_channels_pan) {
    test::RenderOptions o;
    o.initial = {{kSync, 0.0}, {kRate, 3.0}, {kDepth, 1.0}, {kPhase, 180.0}};
    const test::Signal out = test::render(level(48000), o);
    double error = 0.0;
    for (int i = 0; i < 48000; ++i)
        error = std::max(error, std::fabs(out[0][static_cast<size_t>(i)] + out[1][static_cast<size_t>(i)] - 0.5));
    CHECK_MSG(error < 1e-9, "left + right strays from constant by %g", error);
}

TEST(output_at_its_minimum_is_silence) {
    test::RenderOptions o;
    o.initial = {{kOutput, -60.0}};
    const test::Signal out = test::render(test::noise(2, 4800, 5), o);
    CHECK(test::peak(out) == 0.0);
    // Gliding down to it ends in silence too, exactly.
    test::RenderOptions g;
    g.automation = {{1000, kOutput, -60.0}};
    const test::Signal faded = test::render(test::noise(2, 96000, 6), g);
    CHECK(test::peak(faded, 90000) == 0.0);
}

TEST(the_gain_output_reports_the_left_channels_gain) {
    auto engine = std::make_unique<Engine>();
    test::RenderOptions o;
    o.initial = {{kSync, 0.0}, {kRate, 2.0}, {kDepth, 1.0}};
    const int frames = 3000;  // ends a quarter of a cycle in, at its last sample
    test::render(level(frames), o, engine.get());
    const ParamDef& gain = *params::find(kGain);
    const double phase = (frames - 1) * 2.0 / kSampleRate;
    const double reported = toPlain(gain, engine->store().get(static_cast<size_t>(params::indexOf(kGain))));
    CHECK_NEAR(reported, Processor::shapeAt(Processor::kSine, phase), 1e-12);
}

SUPPLE_TEST_MAIN
