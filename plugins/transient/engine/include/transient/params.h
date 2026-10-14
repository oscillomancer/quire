// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <array>
#include "supple/params/params.h"
#include "supple/patch/patch.h"
namespace supple::transient {
inline constexpr const char* kName = SUPPLE_TRANSIENT_DISPLAY_NAME;
enum ParamId : std::uint32_t {
    kAttack,
    kSustain,
    kSpeed,
    kSensitivity,
    kClip,
    kMix,
    kOutput,
    kInputLevel,
    kOutputLevel,
    kGain,
    kNumParams
};
using Values = std::array<double, kNumParams>;
params::Table table();
Values defaults();
std::string formatValue(std::size_t index, double physical, bool compact = false);
const patch::FlatPatchType& patchType();
std::string describe(const Values&, patch::Detail detail = patch::Detail::Changed);
}  // namespace supple::transient
