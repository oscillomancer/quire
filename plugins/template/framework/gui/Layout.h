// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Rectangles in design units, and the arithmetic that lays a panel out
// without hand-computed coordinates.
//
// Cutting takes a strip off one side of a rectangle and returns it, leaving
// the rest, so a layout reads top to bottom as the panel does:
//
//   Rect area = kPanel.inset(16.0f);
//   const Rect title = area.cutTop(24.0f, 8.0f);   // a strip, then an 8 gap
//   const Rect meters = area.cutRight(80.0f, 12.0f);
//   Grid knobs(area, 3, 2, 12.0f);                 // what remains, 3 x 2
//   ui().knob(kDrive, knobs.cell(0, 0));
//   ui().knob(kTone, knobs.cell(1, 0));
//   ui().hslider(kMix, knobs.span(0, 1, 3, 1));    // a whole row
//
// Everything is constexpr, so a layout can be constants at namespace scope.
#pragma once

#include <algorithm>

namespace supple::gui {

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    constexpr bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    constexpr bool contains(const Rect& r) const {
        return r.x >= x && r.y >= y && r.right() <= right() && r.bottom() <= bottom();
    }
    constexpr bool overlaps(const Rect& r) const {
        return x < r.right() && r.x < right() && y < r.bottom() && r.y < bottom();
    }
    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr float cx() const { return x + w * 0.5f; }
    constexpr float cy() const { return y + h * 0.5f; }
    constexpr Rect inset(float d) const { return {x + d, y + d, w - 2.0f * d, h - 2.0f * d}; }
    constexpr Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2.0f * dx, h - 2.0f * dy}; }
    constexpr Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }

    // A strip of `size` off one side, returned; this keeps the rest, less
    // `gap`. A size larger than what is left takes all of it.
    constexpr Rect cutTop(float size, float gap = 0.0f) {
        size = std::clamp(size, 0.0f, h);
        const Rect strip{x, y, w, size};
        const float taken = std::min(h, size + gap);
        y += taken;
        h -= taken;
        return strip;
    }
    constexpr Rect cutBottom(float size, float gap = 0.0f) {
        size = std::clamp(size, 0.0f, h);
        const Rect strip{x, bottom() - size, w, size};
        h -= std::min(h, size + gap);
        return strip;
    }
    constexpr Rect cutLeft(float size, float gap = 0.0f) {
        size = std::clamp(size, 0.0f, w);
        const Rect strip{x, y, size, h};
        const float taken = std::min(w, size + gap);
        x += taken;
        w -= taken;
        return strip;
    }
    constexpr Rect cutRight(float size, float gap = 0.0f) {
        size = std::clamp(size, 0.0f, w);
        const Rect strip{right() - size, y, size, h};
        w -= std::min(w, size + gap);
        return strip;
    }

    // A `width` x `height` rectangle centred in this one.
    constexpr Rect centred(float width, float height) const {
        return {cx() - width * 0.5f, cy() - height * 0.5f, width, height};
    }
    // Part of this one: fractions of its width and height.
    constexpr Rect fraction(float fx, float fy, float fw, float fh) const {
        return {x + w * fx, y + h * fy, w * fw, h * fh};
    }
};

// Equal cells across `area`, `columns` x `rows`, with `gap` between them.
struct Grid {
    Rect area;
    int columns = 1, rows = 1;
    float gapX = 0.0f, gapY = 0.0f;

    constexpr Grid(Rect r, int c, int rw, float gap = 0.0f) : area(r), columns(std::max(1, c)), rows(std::max(1, rw)), gapX(gap), gapY(gap) {}
    constexpr Grid(Rect r, int c, int rw, float gx, float gy)
        : area(r), columns(std::max(1, c)), rows(std::max(1, rw)), gapX(gx), gapY(gy) {}

    constexpr float cellWidth() const { return (area.w - gapX * static_cast<float>(columns - 1)) / static_cast<float>(columns); }
    constexpr float cellHeight() const { return (area.h - gapY * static_cast<float>(rows - 1)) / static_cast<float>(rows); }
    constexpr Rect cell(int column, int row) const { return span(column, row, 1, 1); }
    // `across` columns and `down` rows from (column, row), gaps included.
    constexpr Rect span(int column, int row, int across, int down) const {
        const float w = cellWidth(), h = cellHeight();
        return {area.x + (w + gapX) * static_cast<float>(column), area.y + (h + gapY) * static_cast<float>(row),
                w * static_cast<float>(across) + gapX * static_cast<float>(across - 1),
                h * static_cast<float>(down) + gapY * static_cast<float>(down - 1)};
    }
    // The i-th cell, row by row.
    constexpr Rect operator[](int i) const { return cell(i % columns, i / columns); }
};

// One row of `count` equal cells, or one column.
constexpr Grid row(Rect r, int count, float gap = 0.0f) { return Grid(r, count, 1, gap); }
constexpr Grid column(Rect r, int count, float gap = 0.0f) { return Grid(r, 1, count, gap); }

// Where the contents of widgets::panel(r, title) go: inside its edge, and
// below its title and rule when it has one.
constexpr Rect panelBody(Rect panel, bool titled = true, float padding = 16.0f) {
    Rect body = panel.inset(padding);
    if (titled) body.cutTop(44.0f - padding);
    return body;
}

}  // namespace supple::gui
