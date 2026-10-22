// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "Check.h"
#include "supple/script.h"

namespace supple::script::meter_test {
#include "../script/treeline.cpp"
}  // namespace supple::script::meter_test

namespace {
using Script = supple::script::meter_test::Treeline;

double energyAt(double level) { return std::pow(10.0, (level + 0.691) / 10.0); }
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

SUPPLE_TEST_MAIN
