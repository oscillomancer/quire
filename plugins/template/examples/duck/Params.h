// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter table: the only description of this product's parameters.
// IDs are session ABI: append, never reorder, renumber or reuse.
#pragma once

#include <cstdint>

#include "core/Param.h"

namespace product {

enum ParamId : uint32_t {
    kThreshold = 0,
    kDepth = 1,
    kAttack = 2,
    kRelease = 3,
    kReduction = 4,
    kBypass = 5,
};

inline constexpr supple::ParamDef kParams[] = {
    supple::decibels(kThreshold, "Threshold", -60.0, 0.0, -30.0).in("Key")
        .help("The sidechain's level above which the input ducks."),
    // How far the input is turned down while the key is above the threshold;
    // at the bottom, all the way.
    supple::decibels(kDepth, "Depth", -60.0, 0.0, -12.0).offAtMinimum().in("Duck")
        .help("How far the input is turned down while the key is above the threshold."),
    supple::milliseconds(kAttack, "Attack", 0.1, 100.0, 5.0).in("Duck").help("How fast it ducks."),
    supple::milliseconds(kRelease, "Release", 10.0, 2000.0, 200.0).in("Duck").help("How fast it comes back."),
    supple::decibels(kReduction, "Reduction", -60.0, 0.0, 0.0).output().help("How far the input is turned down now."),
    supple::toggle(kBypass, "Bypass", false).bypass(),
};

}  // namespace product
