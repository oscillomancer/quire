// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Images.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <nanosvg.h>

#include "core/Assets.h"

namespace supple::gui {
namespace {

NVGcolor colourOf(unsigned int abgr, float opacity) {
    return nvgRGBA(static_cast<unsigned char>(abgr), static_cast<unsigned char>(abgr >> 8),
                   static_cast<unsigned char>(abgr >> 16),
                   static_cast<unsigned char>(std::lround(static_cast<float>(abgr >> 24) * opacity)));
}

// nanosvg keeps a gradient's transform from the drawing into the gradient's
// own space; its inverse puts the gradient's ends back in the drawing.
void gradientToDrawing(const float* t, float gx, float gy, float& x, float& y) {
    const float det = t[0] * t[3] - t[2] * t[1];
    if (std::fabs(det) < 1e-12f) {
        x = y = 0.0f;
        return;
    }
    const float inv0 = t[3] / det, inv1 = -t[1] / det, inv2 = -t[2] / det, inv3 = t[0] / det;
    const float inv4 = (t[2] * t[5] - t[3] * t[4]) / det, inv5 = (t[1] * t[4] - t[0] * t[5]) / det;
    x = gx * inv0 + gy * inv2 + inv4;
    y = gx * inv1 + gy * inv3 + inv5;
}

// A fill or stroke paint, or false for none.
bool paintOf(NVGcontext* vg, const NSVGpaint& paint, float opacity, const NVGcolor* tint, NVGpaint& out,
             NVGcolor& colour, bool& solid) {
    auto tinted = [&](NVGcolor c) {
        if (!tint) return c;
        NVGcolor t = *tint;
        t.a *= c.a;
        return t;
    };
    switch (paint.type) {
        case NSVG_PAINT_COLOR:
            colour = tinted(colourOf(paint.color, opacity));
            solid = true;
            return true;
        case NSVG_PAINT_LINEAR_GRADIENT:
        case NSVG_PAINT_RADIAL_GRADIENT: {
            const NSVGgradient& g = *paint.gradient;
            const NVGcolor first = tinted(colourOf(g.stops[0].color, opacity));
            const NVGcolor last = tinted(colourOf(g.stops[g.nstops - 1].color, opacity));
            float x0, y0, x1, y1;
            if (paint.type == NSVG_PAINT_LINEAR_GRADIENT) {
                gradientToDrawing(g.xform, 0.0f, 0.0f, x0, y0);
                gradientToDrawing(g.xform, 0.0f, 1.0f, x1, y1);
                out = nvgLinearGradient(vg, x0, y0, x1, y1, first, last);
            } else {
                gradientToDrawing(g.xform, 0.0f, 0.0f, x0, y0);
                gradientToDrawing(g.xform, 1.0f, 0.0f, x1, y1);
                const float r = std::hypot(x1 - x0, y1 - y0);
                out = nvgRadialGradient(vg, x0, y0, 0.0f, r, first, last);
            }
            solid = false;
            return true;
        }
        default: return false;
    }
}

// Twice the control polygon's signed area, as NanoVG measures it.
float signedArea(const NSVGpath& path) {
    float area = 0.0f;
    for (int i = 2; i < path.npts; ++i) {
        const float* a = &path.pts[0];
        const float* b = &path.pts[(i - 1) * 2];
        const float* c = &path.pts[i * 2];
        area += (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    }
    return area;
}

// A contour as straight lines, each curve in eight.
std::vector<float> outline(const NSVGpath& path) {
    std::vector<float> xy{path.pts[0], path.pts[1]};
    for (int i = 0; i < path.npts - 1; i += 3) {
        const float* p = &path.pts[i * 2];
        for (int k = 1; k <= 8; ++k) {
            const float t = static_cast<float>(k) / 8.0f, u = 1.0f - t;
            const float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
            xy.push_back(a * p[0] + b * p[2] + c * p[4] + d * p[6]);
            xy.push_back(a * p[1] + b * p[3] + c * p[5] + d * p[7]);
        }
    }
    return xy;
}

// Whether (x, y) is inside an outline, closed or not: an odd number of
// its edges cross the ray to the right.
bool encloses(const std::vector<float>& xy, float x, float y) {
    bool in = false;
    const size_t n = xy.size() / 2;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

int pathCount(const NSVGshape& shape) {
    int n = 0;
    for (const NSVGpath* path = shape.paths; path; path = path->next) ++n;
    return n;
}

// Each path is given its winding: NanoVG fills by the nonzero rule over
// those, and would otherwise turn every path solid.
void tracePaths(NVGcontext* vg, const NSVGshape& shape, const unsigned char* solid) {
    nvgBeginPath(vg);
    for (const NSVGpath* path = shape.paths; path; path = path->next, ++solid) {
        nvgMoveTo(vg, path->pts[0], path->pts[1]);
        for (int i = 0; i < path->npts - 1; i += 3) {
            const float* p = &path->pts[i * 2];
            nvgBezierTo(vg, p[2], p[3], p[4], p[5], p[6], p[7]);
        }
        if (path->closed) nvgClosePath(vg);
        nvgPathWinding(vg, *solid ? NVG_SOLID : NVG_HOLE);
    }
}

}  // namespace

Svg::Svg(std::string_view text) {
    std::string copy(text);  // nanosvg parses in place
    image_ = nsvgParse(copy.data(), "px", 96.0f);
    if (image_ && (image_->width <= 0.0f || image_->height <= 0.0f)) {
        nsvgDelete(image_);
        image_ = nullptr;
    }
    if (!image_) return;
    // Each contour's winding, once. Nonzero: the direction it was drawn in,
    // so a counter drawn the other way (the inside of an "o") is a hole.
    // Evenodd: solid when an even number of the shape's other contours
    // enclose it, a hole when odd -- the same picture wherever contours
    // nest without crossing, which is how icons cut their holes.
    for (const NSVGshape* shape = image_->shapes; shape; shape = shape->next) {
        std::vector<std::vector<float>> outlines;
        if (shape->fillRule == NSVG_FILLRULE_EVENODD)
            for (const NSVGpath* path = shape->paths; path; path = path->next) outlines.push_back(outline(*path));
        size_t k = 0;
        for (const NSVGpath* path = shape->paths; path; path = path->next, ++k) {
            bool solid = signedArea(*path) > 0.0f;
            if (!outlines.empty()) {
                int depth = 0;
                for (size_t j = 0; j < outlines.size(); ++j)
                    if (j != k && encloses(outlines[j], path->pts[0], path->pts[1])) ++depth;
                solid = depth % 2 == 0;
            }
            solid_.push_back(solid ? 1 : 0);
        }
    }
}

Svg::~Svg() {
    if (image_) nsvgDelete(image_);
}

float Svg::width() const { return image_ ? image_->width : 0.0f; }
float Svg::height() const { return image_ ? image_->height : 0.0f; }

void Svg::draw(NVGcontext* vg, Rect r, const NVGcolor* tint) const {
    if (!image_ || r.w <= 0.0f || r.h <= 0.0f) return;
    const float scale = std::min(r.w / image_->width, r.h / image_->height);
    nvgSave(vg);
    nvgTranslate(vg, r.cx() - image_->width * scale * 0.5f, r.cy() - image_->height * scale * 0.5f);
    nvgScale(vg, scale, scale);
    size_t next = 0;
    for (const NSVGshape* shape = image_->shapes; shape; shape = shape->next) {
        const unsigned char* windings = solid_.data() + next;
        next += static_cast<size_t>(pathCount(*shape));
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
        NVGpaint paint{};
        NVGcolor colour{};
        bool solid = true;
        const bool strokeFirst = ((shape->paintOrder & 3) == NSVG_PAINT_STROKE);
        for (int pass = 0; pass < 2; ++pass) {
            const bool stroke = (pass == 0) == strokeFirst;
            if (!stroke) {
                if (!paintOf(vg, shape->fill, shape->opacity, tint, paint, colour, solid)) continue;
                tracePaths(vg, *shape, windings);
                if (solid) nvgFillColor(vg, colour);
                else nvgFillPaint(vg, paint);
                nvgFill(vg);
            } else {
                if (shape->strokeWidth <= 0.0f ||
                    !paintOf(vg, shape->stroke, shape->opacity, tint, paint, colour, solid))
                    continue;
                tracePaths(vg, *shape, windings);
                nvgStrokeWidth(vg, shape->strokeWidth);
                nvgLineJoin(vg, shape->strokeLineJoin == NSVG_JOIN_ROUND   ? NVG_ROUND
                                : shape->strokeLineJoin == NSVG_JOIN_BEVEL ? NVG_BEVEL
                                                                           : NVG_MITER);
                nvgLineCap(vg, shape->strokeLineCap == NSVG_CAP_ROUND    ? NVG_ROUND
                               : shape->strokeLineCap == NSVG_CAP_SQUARE ? NVG_SQUARE
                                                                         : NVG_BUTT);
                nvgMiterLimit(vg, shape->miterLimit);
                if (solid) nvgStrokeColor(vg, colour);
                else nvgStrokePaint(vg, paint);
                nvgStroke(vg);
            }
        }
    }
    nvgRestore(vg);
}

const Svg* Images::svg(std::string_view asset) {
    if (const auto it = svgs_.find(asset); it != svgs_.end()) return it->second->valid() ? it->second.get() : nullptr;
    const assets::Asset a = assets::find(asset);
    auto parsed = std::make_unique<Svg>(a ? a.text() : std::string_view{});
    const Svg* result = parsed->valid() ? parsed.get() : nullptr;
    svgs_.emplace(std::string(asset), std::move(parsed));  // a failure is remembered too
    return result;
}

int Images::image(NVGcontext* vg, std::string_view asset) {
    if (context_ != vg) {
        images_.clear();  // another context's handles mean nothing here
        context_ = vg;
    }
    if (const auto it = images_.find(asset); it != images_.end()) return it->second;
    const assets::Asset a = assets::find(asset);
    // NanoVG decodes from memory it does not keep.
    const int handle = a ? nvgCreateImageMem(vg, 0, const_cast<unsigned char*>(a.data), static_cast<int>(a.size)) : 0;
    images_.emplace(std::string(asset), std::max(0, handle));
    return std::max(0, handle);
}

void Images::detach(NVGcontext* vg) {
    if (vg && vg == context_)
        for (const auto& [name, handle] : images_)
            if (handle > 0) nvgDeleteImage(vg, handle);
    images_.clear();
    context_ = nullptr;
}

}  // namespace supple::gui
