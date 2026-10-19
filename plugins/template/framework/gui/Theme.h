// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The colour and geometry tokens every shared widget draws from.
//
// A product supplies one Theme (product/Theme.h) and the widgets take their
// look from it. Add colours a product's own drawing needs there, not here;
// never hard-code a colour in a shared widget to avoid adding a token.
#pragma once

#include <cstdint>

#include <nanovg.h>

namespace supple::gui {

struct Theme {
    // Surfaces, darkest to lightest.
    uint32_t ground;   // window background
    uint32_t surface;  // raised panels
    uint32_t raised;   // resting face of buttons, segments, menus
    uint32_t recess;   // wells: graph floors, meter tracks, text fields
    uint32_t line;     // hairlines and panel edges
    uint32_t lineLit;  // lit edges, emphasised grid lines

    // Text, least to most emphasis.
    uint32_t textFaint;
    uint32_t textMuted;
    uint32_t text;

    uint32_t accent;      // value arcs, active toggles, focus
    uint32_t accent2;     // a second data series: input meters, a second curve
    uint32_t modulation;  // CLAP modulation ranges
    uint32_t alert;       // clipping, over, errors

    // A knob needs four tones of its own; borrowing surface greys is how a
    // product ends up with another product's knobs.
    uint32_t knobTrack;
    uint32_t knobTop;
    uint32_t knobBottom;
    uint32_t knobRim;

    float radius;  // panel corner radius, design units
};

inline NVGcolor rgb(uint32_t v) {
    return nvgRGB(static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 8),
                  static_cast<unsigned char>(v));
}

inline NVGcolor rgba(uint32_t v, float alpha) {
    NVGcolor c = rgb(v);
    c.a = alpha;
    return c;
}

inline NVGcolor withAlpha(NVGcolor c, float alpha) {
    c.a = alpha;
    return c;
}

// Face names, registered per NanoVG context (so per editor, never shared).
// Inter for words, Plex Mono for numbers, Plex Sans Condensed for display,
// and Fraunces, a soft serif, for a product that wants a warmer voice in its
// name and headings (SemiBold) or an aside (Italic).
namespace font {
inline constexpr const char* kUi = "ui";
inline constexpr const char* kUiBold = "ui-semibold";
inline constexpr const char* kMono = "mono";
inline constexpr const char* kMonoMedium = "mono-medium";
inline constexpr const char* kDisplay = "display";
inline constexpr const char* kSerif = "serif";
inline constexpr const char* kSerifItalic = "serif-italic";
}  // namespace font

}  // namespace supple::gui
