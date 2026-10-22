// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "Check.h"
#include "supple/script.h"
#include "supple/script_runtime.h"

namespace supple::script::meter_test {
#include "../script/treeline.cpp"
}  // namespace supple::script::meter_test

namespace {
using Script = supple::script::meter_test::Treeline;

double energyAt(double level) { return std::pow(10.0, (level + 0.691) / 10.0); }

void setAccumulating(Script& script, bool on) {
    supple::script::detail::ParamBinding::setValue(script.accumulate, on ? 1.0 : 0.0, true);
    script.update();
}

void silence(Script& script, int frames, bool playing = false) {
    std::vector<double> left(static_cast<size_t>(frames), 0.0), right(left);
    double* channels[] = {left.data(), right.data()};
    supple::script::Block block;
    block.channels = 2;
    block.frames = frames;
    block.data_ = channels;
    block.transport.playing = playing;
    script.process(block);
}

void tone(Script& script, int frames, int from, double amplitude) {
    std::vector<double> left(static_cast<size_t>(frames)), right(left);
    for (int i = 0; i < frames; ++i)
        left[static_cast<size_t>(i)] = right[static_cast<size_t>(i)] =
            amplitude * std::sin(supple::script::twoPi * 1000.0 * (from + i) / script.rate);
    double* channels[] = {left.data(), right.data()};
    supple::script::Block block;
    block.channels = 2;
    block.frames = frames;
    block.data_ = channels;
    script.process(block);
}

void oldMeterHistory(Script& script) {
    const double energy = energyAt(-15.0);
    script.blockOut.fill(energy);
    script.blockIn.fill(energy);
    script.blocksFilled = 30;
    script.blockCount = script.blockLength / 2;
    script.sumOut = energy * script.blockCount;
    script.sumIn = script.sumOut;
    script.kOut[0].shelf.z1 = 1.0;
    script.outDetect[0].hist.fill(1.0);
    script.outDetect[0].before = 1.0;
    script.outDetect[0].lastPoint = 1.0;
}
}  // namespace

TEST(recent_integrated_keeps_the_absolute_gate_when_the_relative_gate_is_lower) {
    Script script;
    script.recentFilled = 2;
    script.recentShort.fill(-200.0);
    script.recentMomentary[0] = energyAt(-69.5);
    script.recentMomentary[1] = energyAt(-75.0);
    script.recentLoudness();
    CHECK_NEAR(script.integratedNow, -69.5, 1e-9);
}

TEST(recent_integrated_keeps_the_relative_gate_above_the_absolute_gate) {
    Script script;
    script.recentFilled = 2;
    script.recentShort.fill(-200.0);
    script.recentMomentary[0] = energyAt(-20.0);
    script.recentMomentary[1] = energyAt(-40.0);
    script.recentLoudness();
    CHECK_NEAR(script.integratedNow, -20.0, 1e-9);
}

TEST(recent_integrated_is_silent_when_no_block_passes_the_absolute_gate) {
    Script script;
    script.recentFilled = 2;
    script.recentShort.fill(-200.0);
    script.recentMomentary[0] = energyAt(-75.0);
    script.recentMomentary[1] = 0.0;
    script.recentLoudness();
    CHECK_NEAR(script.integratedNow, -120.0, 1e-9);
}

TEST(mode_restart_excludes_old_windows_partial_blocks_and_detector_history_from_totals) {
    Script script;
    script.prepare();
    oldMeterHistory(script);
    setAccumulating(script, true);
    silence(script, script.blockLength / 2);
    CHECK_NEAR(script.integratedNow, -120.0, 1e-9);
    CHECK(script.gatedBlocks == 0);
    CHECK(script.rangeBlocks == 0);
    CHECK_NEAR(script.peakHeld, 0.0, 0.0);
    CHECK(script.overs == 0);
    CHECK_NEAR(script.secondsShown.get(), 0.05, 1e-9);
    // Recent meters still contain their old window and interpolator history.
    CHECK(script.blocksFilled == 30);
    CHECK(script.momentaryNow > -20.0);
    CHECK(script.recentOvers > 0);
    silence(script, 30 * script.blockLength);
    CHECK(script.gatedBlocks == 0);
    CHECK(script.rangeBlocks == 0);
    CHECK_NEAR(script.integratedNow, -120.0, 1e-9);
    CHECK_NEAR(script.secondsShown.get(), 3.05, 1e-9);
}

TEST(play_restart_excludes_old_weighting_and_peak_detector_history_from_totals) {
    Script script;
    script.prepare();
    setAccumulating(script, true);
    oldMeterHistory(script);
    silence(script, 4 * script.blockLength, true);
    CHECK(script.gatedBlocks == 0);
    CHECK(script.rangeBlocks == 0);
    CHECK_NEAR(script.integratedNow, -120.0, 1e-9);
    CHECK_NEAR(script.peakHeld, 0.0, 0.0);
    CHECK(script.overs == 0);
    CHECK_NEAR(script.secondsShown.get(), 0.4, 1e-9);
}

TEST(restarted_totals_match_a_fresh_measurement_including_the_first_partial_rolling_block) {
    Script fresh, restarted;
    fresh.prepare();
    restarted.prepare();
    oldMeterHistory(restarted);
    setAccumulating(fresh, true);
    setAccumulating(restarted, true);
    // The first 50 ms must contribute even though the old rolling block
    // ends halfway through the accumulated measurement's first block.
    for (Script* script : {&fresh, &restarted}) {
        tone(*script, 2400, 0, 0.3);
        tone(*script, 16800, 2400, 0.03);
    }
    CHECK(fresh.gatedBlocks == 1);
    CHECK(restarted.gatedBlocks == fresh.gatedBlocks);
    CHECK(restarted.rangeBlocks == 0);
    CHECK_NEAR(restarted.integratedNow, fresh.integratedNow, 1e-12);
    CHECK_NEAR(restarted.peakHeld, fresh.peakHeld, 0.0);
    for (Script* script : {&fresh, &restarted}) tone(*script, 129600, 19200, 0.03);
    CHECK(fresh.rangeBlocks == 2);
    CHECK(restarted.rangeBlocks == fresh.rangeBlocks);
    CHECK(restarted.gatedBlocks == fresh.gatedBlocks);
    CHECK_NEAR(restarted.integratedNow, fresh.integratedNow, 1e-12);
    CHECK_NEAR(restarted.rangeNow, fresh.rangeNow, 0.0);
    CHECK_NEAR(restarted.peakHeld, fresh.peakHeld, 0.0);
    CHECK(restarted.overs == fresh.overs);
    CHECK_NEAR(restarted.secondsShown.get(), 3.1, 1e-9);
}

SUPPLE_TEST_MAIN
