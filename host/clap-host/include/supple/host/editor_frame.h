// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A bridge-owned editor window's content frame (macOS). Kept apart from
// clap_host.h so Objective-C++ platform code can use it.
#pragma once

#include <cstdint>

namespace supple::host {

// Points, origin at the top left of the primary display: the space SDL uses
// for window positions, so it is stored like any other editor placement.
struct EditorFrame {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 0, height = 0;
    bool operator==(const EditorFrame&) const = default;
};

}  // namespace supple::host
