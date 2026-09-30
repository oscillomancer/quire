// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Delay GUI — shared drawing primitives. Product colours and fonts live in Theme.h.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include <nanovg.h>

#include "Theme.h"

namespace supple::delay::gui {

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

inline void pathRoundRect(NVGcontext* vg, float x, float y, float w, float h, float r) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, r);
}

inline void fillRoundRect(NVGcontext* vg, float x, float y, float w, float h, float r,
                          NVGcolor c) {
    pathRoundRect(vg, x, y, w, h, r);
    nvgFillColor(vg, c);
    nvgFill(vg);
}

inline void strokeRoundRect(NVGcontext* vg, float x, float y, float w, float h, float r,
                            NVGcolor c, float lw = 1.0f) {
    pathRoundRect(vg, x + lw * 0.5f, y + lw * 0.5f, w - lw, h - lw, r);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, lw);
    nvgStroke(vg);
}

inline void hairline(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor c,
                     float lw = 1.0f) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, x0, y0);
    nvgLineTo(vg, x1, y1);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, lw);
    nvgStroke(vg);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

enum class Align { Left, Center, Right };

// Draw a whole run so glyphs share one rasterization origin. Inter supplies
// the UI labels; Plex Mono remains reserved for values and axis numbers.
inline void label(NVGcontext* vg, const char* text, float x, float y, float size,
                  NVGcolor c, Align align = Align::Left, bool strong = false) {
    nvgFontFace(vg, strong ? kFontUiSemi : kFontUi);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, (align == Align::Center ? NVG_ALIGN_CENTER
                      : align == Align::Right ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) |
                         NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, c);
    nvgText(vg, x, y, text, nullptr);
}

// Numerals and anything the user reads as data. Tabular by virtue of the face.
inline void num(NVGcontext* vg, const char* text, float x, float y, float size, NVGcolor c,
                Align align = Align::Left, bool medium = true) {
    nvgFontFace(vg, medium ? kFontMonoMed : kFontMono);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, (align == Align::Center ? NVG_ALIGN_CENTER
                      : align == Align::Right ? NVG_ALIGN_RIGHT
                                              : NVG_ALIGN_LEFT) |
                         NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, c);
    nvgText(vg, x, y, text, nullptr);
}

// ---------------------------------------------------------------------------
// Glow
// ---------------------------------------------------------------------------
//
// Canvas shadowBlur has no NanoVG equivalent. Restroking the same path wider
// and fainter is cheap, and on a dark ground it is indistinguishable.

inline void glowStroke(NVGcontext* vg, NVGcolor c, float width, int passes = 3) {
    for (int i = passes; i >= 1; --i) {
        NVGcolor g = c;
        g.a = 0.11f * (float)i / (float)passes;
        nvgStrokeColor(vg, g);
        nvgStrokeWidth(vg, width + (float)i * 1.8f);
        nvgStroke(vg);
    }
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
}

inline void glowDot(NVGcontext* vg, float x, float y, float r, NVGcolor c) {
    NVGpaint p = nvgRadialGradient(vg, x, y, r * 0.5f, r * 3.2f, nvgRGBAf(c.r, c.g, c.b, 0.45f),
                                   nvgRGBAf(c.r, c.g, c.b, 0.0f));
    nvgBeginPath(vg);
    nvgCircle(vg, x, y, r * 3.2f);
    nvgFillPaint(vg, p);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, x, y, r);
    nvgFillColor(vg, c);
    nvgFill(vg);
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

constexpr float kKnobA0 = 3.14159265f * 0.75f;
constexpr float kKnobA1 = 3.14159265f * 2.25f;

inline void knob(NVGcontext* vg, float cx, float cy, float r, float t,
                 const char* nameRaw, const char* value, const char* unit, bool big,
                 bool hot = false, bool bipolar = false) {
    const float a = kKnobA0 + (kKnobA1 - kKnobA0) * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);

    // The shadow extends past the track; the darker seat separates the face
    // from that track, so the ring reads as a recess rather than a flat stroke.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy + 3.0f, r + 9.0f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy + 3.0f, r - 7.0f, r + 9.0f,
                                      rgba(col::ink, 0.78f), rgba(col::ink, 0.0f)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r + 1.5f);
    nvgFillColor(vg, rgb(col::well));
    nvgFill(vg);

    // Dark recessed track, with a dim rim outside the active arc.
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r, kKnobA0, kKnobA1, NVG_CW);
    nvgStrokeColor(vg, rgb(col::knobWell));
    nvgStrokeWidth(vg, 5.0f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
    nvgStrokeColor(vg, rgba(col::edgeHi, 0.30f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    // Bevel under the face, then a vertical gradient and off-centre soft light.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy + 1.0f, r - 5.0f);
    nvgFillPaint(vg, nvgLinearGradient(vg, cx, cy - r, cx, cy + r,
                                      rgb(col::knobRim), rgb(col::knobWell)));
    nvgFill(vg);
    NVGpaint body = nvgLinearGradient(vg, cx, cy - r + 6.0f, cx, cy + r - 5.0f,
                                     rgb(col::knobTop), rgb(col::knobBot));
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r - 7.5f);
    nvgFillPaint(vg, body);
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(col::knobRim, hot ? 1.0f : 0.75f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r - 8.0f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx - r * 0.25f, cy - r * 0.39f,
                                      0.0f, r * 1.10f,
                                      rgba(col::bone, hot ? 0.16f : 0.11f),
                                      rgba(col::bone, 0.0f)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r - 7.5f, 3.35f, 6.06f, NVG_CW);
    nvgStrokeColor(vg, rgba(col::bone, hot ? 0.24f : 0.15f));
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r - 7.5f, 0.20f, 2.94f, NVG_CW);
    nvgStrokeColor(vg, rgba(col::ink, 0.48f));
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);

    // value arc, with a little bloom so it reads as lit rather than painted
    if (bipolar ? std::abs(t - .5f) > .0002f : a > kKnobA0 + 0.001f) {
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r, bipolar ? (kKnobA0 + kKnobA1) * .5f : kKnobA0, a,
               bipolar && t < .5f ? NVG_CCW : NVG_CW);
        nvgLineCap(vg, NVG_ROUND);
        glowStroke(vg, rgb(col::accent), hot ? 3.2f : 2.8f, 2);
    }

    // A light dot sits inside the face, with a dark outline for separation
    // from both the highlight and the centre value.
    const float px = cx + std::cos(a) * (r - 15.0f);
    const float py = cy + std::sin(a) * (r - 15.0f);
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, 3.8f);
    nvgFillColor(vg, rgba(col::well, 0.85f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, hot ? 2.8f : 2.45f);
    nvgFillColor(vg, rgb(col::bone));
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(col::bone, 0.28f));
    nvgStrokeWidth(vg, 0.8f);
    nvgStroke(vg);

    label(vg, nameRaw, cx, cy + r + 13.0f, 10.5f, rgb(col::dust), Align::Center, true);

    const bool hasUnit = unit && unit[0];
    num(vg, value, cx, cy - (hasUnit ? 5.0f : 0.0f), big ? 15.0f : 12.5f, rgb(col::bone),
        Align::Center);
    if (hasUnit)
        num(vg, unit, cx, cy + 11.0f, 8.5f, rgb(col::dust), Align::Center, false);
}

// showDot: a lit dot on the left, for a control that is simply on or off. A
// pill that is one of a set does not need it -- the fill already says which is
// chosen -- and dropping it gives the text the whole pill, which the long
// labels need: several of them do not fit beside a dot at any size worth
// reading.
inline void pill(NVGcontext* vg, float x, float y, float w, float h, const char* text, bool on,
                 NVGcolor accent, bool showDot = true, bool hot = false) {
    constexpr float radius = 5.0f;
    nvgBeginPath(vg);
    nvgRect(vg, x - 7, y - 5, w + 14, h + 15);
    nvgRoundedRect(vg, x, y, w, h, radius);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, nvgBoxGradient(vg, x, y + 2, w, h, radius, 7,
                                   rgba(col::ink, 0.65f), rgba(col::ink, 0)));
    nvgFill(vg);
    pathRoundRect(vg, x, y, w, h, radius);
    nvgFillPaint(vg, on
        ? nvgLinearGradient(vg, x, y, x, y + h,
                            nvgRGBAf(accent.r, accent.g, accent.b, hot ? 0.35f : 0.27f),
                            nvgRGBAf(accent.r, accent.g, accent.b, 0.11f))
        : nvgLinearGradient(vg, x, y, x, y + h,
                            rgb(col::knobTop), rgb(col::panel)));
    nvgFill(vg);
    strokeRoundRect(vg, x, y, w, h, radius,
                    on ? nvgRGBAf(accent.r, accent.g, accent.b, hot ? 0.92f : 0.65f)
                       : rgba(col::edgeHi, hot ? 1.0f : 0.76f));
    hairline(vg, x + 7, y + 1.5f, x + w - 7, y + 1.5f,
             on ? nvgRGBAf(accent.r, accent.g, accent.b, 0.26f)
                : rgba(col::bone, hot ? 0.19f : 0.10f));

    if (showDot) {
        const float ledX = x + 14.0f, ledY = y + h * 0.5f;
        nvgBeginPath(vg);
        nvgCircle(vg, ledX, ledY, 5.0f);
        nvgFillColor(vg, rgb(col::well));
        nvgFill(vg);
        nvgStrokeColor(vg, rgb(col::edgeHi));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        if (on) glowDot(vg, ledX, ledY, hot ? 3.0f : 2.65f, accent);
        else {
            nvgBeginPath(vg);
            nvgCircle(vg, ledX, ledY, 2.55f);
            nvgFillColor(vg, rgb(hot ? col::dust : col::dim));
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgCircle(vg, ledX - 0.7f, ledY - 0.8f, 0.7f);
            nvgFillColor(vg, rgba(col::bone, 0.48f));
            nvgFill(vg);
        }
    }

    // Fit the whole text run to the pill; never drop below a readable size.
    const float inset = showDot ? 28.0f : 8.0f;
    const float avail = w - inset - (showDot ? 7.0f : 8.0f);
    float size = 11.0f;
    nvgFontFace(vg, kFontUiSemi);
    nvgFontSize(vg, size);
    const float tw = nvgTextBounds(vg, 0, 0, text, nullptr, nullptr);
    if (tw > avail && tw > 0.0f) size = std::max(9.0f, size * avail / tw);

    const NVGcolor c = on || hot ? rgb(col::bone) : rgb(col::dust);
    if (showDot) label(vg, text, x + inset, y + h * 0.5f + 0.5f, size, c, Align::Left, true);
    else label(vg, text, x + w * 0.5f, y + h * 0.5f + 0.5f, size, c, Align::Center, true);
}

// Raised panel with a recessed floor, soft outside shadow and lit inner edge.
inline void cell(NVGcontext* vg, float x, float y, float w, float h, const char* title) {
    nvgBeginPath(vg);
    nvgRect(vg, x - 12, y - 9, w + 24, h + 25);
    nvgRoundedRect(vg, x, y, w, h, 5);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, nvgBoxGradient(vg, x, y + 4, w, h, 5, 12,
                                   rgba(col::ink, 0.7f), rgba(col::ink, 0)));
    nvgFill(vg);
    pathRoundRect(vg, x, y, w, h, 5.0f);
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y, x, y + h,
                                      rgb(col::panel), rgb(col::well)));
    nvgFill(vg);
    strokeRoundRect(vg, x, y, w, h, 5.0f, rgb(col::edgeHi));
    hairline(vg, x + 12, y + 25.5f, x + w - 12, y + 25.5f, rgba(col::edgeHi, 0.52f));
    label(vg, title, x + 13.0f, y + 14.0f, 11.0f, rgb(col::dust), Align::Left, true);
}

// Section heading with a rule running out to the right of the words.
inline void section(NVGcontext* vg, float x, float y, float w, const char* title,
                    float rightPad = 0.0f) {
    label(vg, title, x, y, 11.0f, rgb(col::accent), Align::Left, true);
    nvgFontFace(vg, kFontUiSemi);
    nvgFontSize(vg, 11.0f);
    const float tw = nvgTextBounds(vg, 0, 0, title, nullptr, nullptr) + 10.0f;
    hairline(vg, x + tw, y + 0.5f, x + w - rightPad, y + 0.5f, rgb(col::edge));
}

}  // namespace supple::delay::gui
