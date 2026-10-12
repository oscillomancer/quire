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
    kShape = 0,
    kStrum = 1,
};

// Custom plays the intervals typed into the Intervals property.
inline constexpr const char* kShapeLabels[] = {"Octave", "Fifth", "Major", "Minor", "Sus4", "7th", "Custom"};
inline constexpr int kCustomShape = 6;

inline constexpr supple::ParamDef kParams[] = {
    supple::choice(kShape, "Shape", kShapeLabels, 2).help("The chord each note becomes, built up from it."),
    // The time between each note of the chord and the next, lowest first.
    supple::milliseconds(kStrum, "Strum", 0.0, 100.0, 0.0).help("The time between each note of the chord and the next."),
};

}  // namespace product
