// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// An editor drawn without a window (gui/Offscreen.h; a layer's
// Instance::drawEditor): 8-bit RGBA, rows from the top, nothing between
// them. And what a pointer does to the editor before it is drawn, so a
// picture can show (and a test or an agent check) what a click, a drag or
// a turn of the wheel does.
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

// One thing the pointer does, at (x, y) in the editor's design units (the
// picture's pixels at scale 1), with the main button: a drag is a Press,
// Moves, and a Release. What it edits reaches the host as a user's edits do.
struct PointerInput {
    enum class Kind : uint8_t { Move, Press, Release, Wheel, Leave };
    Kind kind = Kind::Move;
    float x = 0.0f, y = 0.0f;
    float wheel = 0.0f;        // Wheel: notches turned, positive up
    bool doubleClick = false;  // Press: the second press of a double click
    bool shift = false, ctrl = false, alt = false;
};

}  // namespace supple
