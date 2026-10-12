// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Factory presets (see core/Preset.h): parameters in plain units, properties
// as text. A preset's name is what hosts remember it by.
#pragma once

#include "core/Preset.h"
#include "Params.h"
#include "Properties.h"

namespace product {

inline constexpr supple::Preset kPresets[] = {
    {"Power", "Basic", {{kShape, 1}}},
    {"Octaves", "Basic", {{kShape, 0}}},
    {"Major Strum", "Strummed", {{kShape, 2}, {kStrum, 30.0}}},
    {"Slow Seventh", "Strummed", {{kShape, 5}, {kStrum, 80.0}}},
    {"Minor Seventh", "Custom", {{kShape, 6}}, {{kIntervals, "0 3 7 10"}}},
    {"Add Nine", "Custom", {{kShape, 6}, {kStrum, 15.0}}, {{kIntervals, "0 4 7 14"}}},
};

}  // namespace product
