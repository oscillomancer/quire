// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#pragma once

#include <nanovg.h>

namespace eq::gui {

constexpr float kDesignW = 900.0f;
constexpr float kDesignH = 560.0f;
constexpr float kHeaderH = 62.0f;
constexpr float kMeterTop = 98.0f;
constexpr float kMeterH = 214.0f;
constexpr float kControlsTop = 350.0f;
constexpr float kFooterTop = 446.0f;
constexpr float kMargin = 30.0f;
constexpr int kHistoryColumns = 540;
constexpr float kHistorySeconds = 3.0f;

inline NVGcolor rgb(unsigned v) {
    return nvgRGB(static_cast<unsigned char>(v >> 16),
                  static_cast<unsigned char>(v >> 8),
                  static_cast<unsigned char>(v));
}
inline NVGcolor rgba(unsigned v, float a) { auto c = rgb(v); c.a = a; return c; }

namespace col {
constexpr unsigned ink = 0x090B10;
constexpr unsigned panel = 0x121620;
constexpr unsigned well = 0x07090D;
constexpr unsigned edge = 0x1F2633;
constexpr unsigned edgeHi = 0x2D3746;
constexpr unsigned bone = 0xEAE5DA;
constexpr unsigned dust = 0xA6ACB6;
constexpr unsigned dim = 0x737C89;
constexpr unsigned accent = 0xD9A448;
constexpr unsigned cool = 0x71A7A0;
constexpr unsigned alert = 0xC65C4F;

// Knob tones. The shared knob widget in Widgets.h requires all four, so every
// product's palette has to define them -- see CONVENTIONS.md.
constexpr unsigned knobWell = 0x121821;  // recessed track
constexpr unsigned knobTop = 0x36414F;   // top-lit body
constexpr unsigned knobBot = 0x141B24;   // shadowed body
constexpr unsigned knobRim = 0x3E4B5A;   // bevel around the body
}

constexpr const char* kFontMono = "eq-mono";
constexpr const char* kFontMonoMed = "eq-mono-medium";
constexpr const char* kFontDisplay = "eq-display";
constexpr const char* kFontUi = "eq-ui";
constexpr const char* kFontUiSemi = "eq-ui-semibold";

}  // namespace eq::gui
