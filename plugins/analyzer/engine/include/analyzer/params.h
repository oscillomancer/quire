// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once

#include <array>
#include "supple/params/params.h"
#include "supple/patch/patch.h"

namespace supple::analyzer {
inline constexpr char kName[] = SUPPLE_ANALYZER_DISPLAY_NAME;
inline constexpr char kPluginId[] = "com.suppleaudio.analyzer";
inline constexpr std::uint32_t kStateMagic = 0x51414E31;  // QAN1

enum ParamId : std::uint32_t {
    kWindow,
    kReset,
    kHold,
    kIntegrated,
    kShortTerm,
    kMomentary,
    kLra,
    kTruePeak,
    kCorrelation,
    kWidth,
    kBalance,
    kCentroid,
    kNumParams
};
using Values = std::array<double, kNumParams>;
params::Table table();
Values defaults();
const patch::FlatPatchType& patchType();
std::string describe(const Values& values, bool changedOnly = true);
double windowSeconds(double choice);
}  // namespace supple::analyzer
