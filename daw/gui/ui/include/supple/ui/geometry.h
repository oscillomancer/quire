// Design-unit geometry for the toolkit (docs/specs/gui.md §2). One design
// unit is one pixel at scale 1; the window's pixel ratio maps the rest.
#pragma once

#include <algorithm>
#include <cmath>

namespace supple::ui {

struct Point {
    float x = 0, y = 0;
};

struct Size {
    float w = 0, h = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < right() && p.y < bottom(); }
    Rect intersect(const Rect& o) const {
        const float l = std::max(x, o.x), t = std::max(y, o.y);
        const float r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return r > l && b > t ? Rect{l, t, r - l, b - t} : Rect{};
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const float l = std::min(x, o.x), t = std::min(y, o.y);
        return {l, t, std::max(right(), o.right()) - l, std::max(bottom(), o.bottom()) - t};
    }
    Rect inset(float d) const {
        return {x + d, y + d, std::max(0.0f, w - 2 * d), std::max(0.0f, h - 2 * d)};
    }
    // Expands outward to whole pixels at `ratio` (damage must cover partial pixels).
    Rect snapOut(float ratio) const {
        const float l = std::floor(x * ratio) / ratio, t = std::floor(y * ratio) / ratio;
        const float r = std::ceil(right() * ratio) / ratio, b = std::ceil(bottom() * ratio) / ratio;
        return {l, t, r - l, b - t};
    }
    bool operator==(const Rect&) const = default;
};

}  // namespace supple::ui
