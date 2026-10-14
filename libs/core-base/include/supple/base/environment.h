// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Environment flags (main/test threads only; never on the audio path).
#pragma once

#include <cstdlib>

namespace supple {

// Opt-in requires exactly "1"; unset, empty and all other values disable it.
inline bool environmentFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
}

}  // namespace supple
