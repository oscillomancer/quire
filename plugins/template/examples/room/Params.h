// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter table. IDs are session ABI: append, never reorder, renumber
// or reuse.
#pragma once

#include <cstdint>

#include "core/Param.h"

namespace product {

enum ParamId : uint32_t {
    kMix = 0,
    kPredelay = 1,
    kTone = 2,
    kOutput = 3,
    kBypass = 4,
};

inline constexpr supple::ParamDef kParams[] = {
    supple::percent(kMix, "Mix", 30.0).modulatable().smoothed(20.0)
        .help("How much of the room is heard; the rest is the dry signal."),
    supple::milliseconds(kPredelay, "Pre-delay", 0.0, 200.0, 10.0).modulatable().smoothed(50.0)
        .help("Time before the room answers: the distance to its nearest wall."),
    supple::frequency(kTone, "Tone", 1000.0, 20000.0, 12000.0).modulatable()
        .help("A low-pass on the room alone, to darken it."),
    supple::decibels(kOutput, "Output", -24.0, 12.0, 0.0).modulatable().smoothed(20.0)
        .help("The level out, after the mix."),
    supple::toggle(kBypass, "Bypass", false).bypass(),
};

}  // namespace product
