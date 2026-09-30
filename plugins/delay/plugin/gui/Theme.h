// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#pragma once

#include <nanovg.h>

namespace delay::gui {

constexpr float kDesignW = 820.0f;
constexpr float kDesignH = 480.0f;
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
constexpr unsigned ink = 0x080A0D;
constexpr unsigned panel = 0x11151C;
constexpr unsigned well = 0x06080B;
constexpr unsigned edge = 0x1E2530;
constexpr unsigned edgeHi = 0x2B3441;
constexpr unsigned bone = 0xE8E4DC;
constexpr unsigned dust = 0xA4AAB3;
constexpr unsigned dim = 0x747D89;
constexpr unsigned accent = 0xE8944A;
constexpr unsigned cool = 0x71A7A0;
constexpr unsigned alert = 0xC25B4E;

// Knob tones. The shared knob widget in Widgets.h requires all four, so every
// product's palette has to define them -- see CONVENTIONS.md.
constexpr unsigned knobWell = 0x121820;  // recessed track
constexpr unsigned knobTop = 0x35404D;   // top-lit body
constexpr unsigned knobBot = 0x131A23;   // shadowed body
constexpr unsigned knobRim = 0x3D4A58;   // bevel around the body
}

constexpr const char* kFontMono = "delay-mono";
constexpr const char* kFontMonoMed = "delay-mono-medium";
constexpr const char* kFontDisplay = "delay-display";
constexpr const char* kFontUi = "delay-ui";
constexpr const char* kFontUiSemi = "delay-ui-semibold";

}  // namespace delay::gui
