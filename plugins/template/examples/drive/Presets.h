// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Factory presets (see core/Preset.h): each sets what it names, in the
// units the editor shows, and leaves the rest at their defaults. A preset's
// name is what hosts remember it by.
#pragma once

#include "core/Preset.h"
#include "Params.h"

namespace product {

inline constexpr supple::Preset kPresets[] = {
    {"Warm", "Gentle", {{kDrive, 6.0}, {kTone, 8000.0}, {kOutput, -3.0}}},
    {"Crunch", "Hard", {{kDrive, 24.0}, {kShape, 1}, {kTone, 6000.0}, {kOutput, -10.0}}},
    {"Folded", "Hard", {{kDrive, 18.0}, {kShape, 2}, {kMix, 0.6}, {kOutput, -8.0}}},
};

}  // namespace product
