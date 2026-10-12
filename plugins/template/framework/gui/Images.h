// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Pictures for the editor, from the product's assets (core/Assets.h).
//
// An SVG is parsed once (nanosvg) and drawn as NanoVG paths: crisp at any
// scale, and optionally in one colour, so an icon follows the theme.
// Shapes, strokes and opacity are drawn as written, under either fill rule
// (evenodd exactly where contours nest without crossing one another, as an
// icon's holes do). Gradients use their first and last stops; text, images,
// masks, filters and dashes are not drawn. Convert text to paths before
// export.
//
// A PNG or JPEG becomes a NanoVG image (NanoVG's own stb_image), one per
// drawing context, made when first drawn and freed when the view goes.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nanovg.h>

#include "gui/Layout.h"

struct NSVGimage;

namespace supple::gui {

class Svg {
  public:
    // Parses the text of an SVG; valid() says whether it could.
    explicit Svg(std::string_view text);
    ~Svg();
    Svg(const Svg&) = delete;
    Svg& operator=(const Svg&) = delete;

    bool valid() const { return image_ != nullptr; }
    float width() const;
    float height() const;
    // Fitted inside `r`, keeping its proportions, centred. With a tint,
    // every fill and stroke is that colour, at the alpha the SVG gave it.
    void draw(NVGcontext* vg, Rect r, const NVGcolor* tint = nullptr) const;

  private:
    NSVGimage* image_ = nullptr;
    std::vector<unsigned char> solid_;  // each path's winding, shape by shape: 1 solid, 0 a hole
};

// What one editor has loaded: SVGs parsed (shared by every context), and
// images, which belong to one context.
class Images {
  public:
    // Null when there is no such asset or it does not parse.
    const Svg* svg(std::string_view asset);
    // A NanoVG image for this context; 0 when there is none.
    int image(NVGcontext* vg, std::string_view asset);
    // The context is going away: its images go with it.
    void detach(NVGcontext* vg);

  private:
    std::map<std::string, std::unique_ptr<Svg>, std::less<>> svgs_;
    std::map<std::string, int, std::less<>> images_;
    NVGcontext* context_ = nullptr;
};

}  // namespace supple::gui
