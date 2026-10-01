#include "supple/ui/canvas.h"

#include <nanovg.h>

#include <string>

namespace supple::ui {
namespace {
NVGcolor nvg(Color c) { return nvgRGBAf(c.r, c.g, c.b, c.a); }
}  // namespace

int Canvas::face(Font f) const {
    switch (f) {
        case Font::Ui:
            return fonts_.ui;
        case Font::UiBold:
            return fonts_.uiBold;
        case Font::Mono:
            return fonts_.mono;
        case Font::MonoBold:
            return fonts_.monoBold;
    }
    return fonts_.ui;
}

void Canvas::fillRect(const Rect& r, Color c) {
    nvgBeginPath(vg_);
    nvgRect(vg_, r.x, r.y, r.w, r.h);
    nvgFillColor(vg_, nvg(c));
    nvgFill(vg_);
}

void Canvas::fillRound(const Rect& r, float radius, Color c) {
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, r.x, r.y, r.w, r.h, radius);
    nvgFillColor(vg_, nvg(c));
    nvgFill(vg_);
}

void Canvas::strokeRect(const Rect& r, Color c, float width) {
    nvgBeginPath(vg_);
    // Half-unit inset so a 1-unit stroke covers whole pixels at scale 1.
    nvgRect(vg_, r.x + width / 2, r.y + width / 2, r.w - width, r.h - width);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
}

void Canvas::strokeRound(const Rect& r, float radius, Color c, float width) {
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, r.x + width / 2, r.y + width / 2, r.w - width, r.h - width, radius);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
}

void Canvas::line(Point a, Point b, Color c, float width) {
    nvgBeginPath(vg_);
    nvgMoveTo(vg_, a.x, a.y);
    nvgLineTo(vg_, b.x, b.y);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
}

float Canvas::textWidth(std::string_view s, Font font, float size) const {
    nvgFontFaceId(vg_, face(font));
    nvgFontSize(vg_, size);
    return nvgTextBounds(vg_, 0, 0, s.data(), s.data() + s.size(), nullptr);
}

void Canvas::text(const Rect& box, std::string_view s, Font font, float size, Color c, int align) {
    nvgFontFaceId(vg_, face(font));
    nvgFontSize(vg_, size);
    nvgFillColor(vg_, nvg(c));
    int a = 0;
    a |= (align & kCenter) ? NVG_ALIGN_CENTER : (align & kRight) ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT;
    a |= (align & kTop)        ? NVG_ALIGN_TOP
         : (align & kBottom)   ? NVG_ALIGN_BOTTOM
         : (align & kBaseline) ? NVG_ALIGN_BASELINE
                               : NVG_ALIGN_MIDDLE;
    nvgTextAlign(vg_, a);
    const float x = (align & kCenter) ? box.x + box.w / 2 : (align & kRight) ? box.right() : box.x;
    const float y = (align & kTop) ? box.y : (align & kBottom) ? box.bottom() : box.y + box.h / 2;
    std::string shown(s);
    if (textWidth(shown, font, size) > box.w && box.w > 0) {
        // Shorten to fit, ending in an ellipsis (U+2026).
        const std::string ellipsis = "\xE2\x80\xA6";
        while (!shown.empty() && textWidth(shown + ellipsis, font, size) > box.w) {
            shown.pop_back();
            while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80)
                shown.pop_back();  // keep UTF-8 sequences whole
        }
        shown += ellipsis;
    }
    nvgText(vg_, x, y, shown.c_str(), nullptr);
}

void Canvas::save() { nvgSave(vg_); }
void Canvas::restore() { nvgRestore(vg_); }
void Canvas::clip(const Rect& r) { nvgIntersectScissor(vg_, r.x, r.y, r.w, r.h); }

}  // namespace supple::ui
