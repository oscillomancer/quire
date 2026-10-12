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
    {"Gentle Eighths", "Tremolo", {{kSync, 1}, {kDivision, supple::noteValueOf("1/8")}, {kDepth, 0.3}}},
    {"Quarter Chop", "Tremolo", {{kSync, 1}, {kDivision, supple::noteValueOf("1/4")}, {kDepth, 1.0}, {kShape, 2}}},
    {"Slow Swell", "Tremolo", {{kSync, 0}, {kRate, 0.25}, {kDepth, 0.7}}},
    {"Wide Pan", "Auto-pan", {{kSync, 1}, {kDivision, supple::noteValueOf("1/2")}, {kDepth, 0.8}, {kPhase, 180.0}}},
    {"Fast Spin", "Auto-pan", {{kSync, 0}, {kRate, 6.0}, {kDepth, 0.6}, {kPhase, 180.0}, {kShape, 1}}},
};

}  // namespace product
