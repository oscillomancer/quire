// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// An editor drawn without a window (gui/Offscreen.h; a layer's
// Instance::drawEditor): 8-bit RGBA, rows from the top, nothing between
// them.
#pragma once

#include <cstdint>
#include <vector>

namespace supple {

struct Picture {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4 bytes

    // The pixel at (x, y), as 0xRRGGBB; 0 outside the picture.
    uint32_t rgb(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        return static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
    }
};

}  // namespace supple
