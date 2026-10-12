// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// State that is not a parameter (see core/Property.h). IDs are session ABI,
// like parameter IDs.
//
// What this example shows: a file. The impulse response is a path, and the
// framework does the rest -- relocates it if the session moved, loads it off
// the audio thread with the processor's loadFile(), reports it missing.
#pragma once

#include <cstdint>

#include "core/Property.h"

namespace product {

enum PropertyId : uint32_t {
    kImpulse = 0,
};

inline constexpr supple::PropertyDef kProperties[] = {
    // Empty: the processor's own room.
    supple::property(kImpulse, "Impulse").audioFile(),
};

}  // namespace product
