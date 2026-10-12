// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// State that is not a parameter (see core/Property.h). IDs are session ABI,
// like parameter IDs.
//
// What this example shows: text the processor reads (the custom chord, saved
// in presets) and text only the session keeps (a label for this instance,
// which no preset changes).
#pragma once

#include <cstdint>

#include "core/Property.h"

namespace product {

enum PropertyId : uint32_t {
    kIntervals = 0,
    kLabel = 1,
};

inline constexpr supple::PropertyDef kProperties[] = {
    // The Custom shape: semitones above the played note, in strum order.
    supple::property(kIntervals, "Intervals", "0 4 7 11").maxBytes(64),
    supple::property(kLabel, "Label").perProject().maxBytes(48),
};

}  // namespace product
