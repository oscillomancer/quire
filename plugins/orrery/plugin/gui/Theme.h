// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Orrery's palette, type and panel geometry. The token names are a contract
// with the shared Widgets.h (see CONVENTIONS.md); the layout constants belong
// to this product's editor.
#pragma once

#include <nanovg.h>

namespace orrery::gui {

constexpr float kDesignW = 1120.0f;
constexpr float kDesignH = 700.0f;
constexpr float kHeaderH = 58.0f;
constexpr float kMargin = 20.0f;

// Left: the patch as a signal-flow diagram and the modulators; right: the
// selected module's parameters and the modulation routes.
constexpr float kFlowX = kMargin, kFlowY = 74.0f, kFlowW = 520.0f, kFlowH = 348.0f;
constexpr float kModX = kMargin, kModY = 436.0f, kModW = 520.0f, kModH = 228.0f;
constexpr float kInspX = 556.0f, kInspY = 74.0f, kInspW = 544.0f, kInspH = 348.0f;
constexpr float kRouteX = 556.0f, kRouteY = 436.0f, kRouteW = 544.0f, kRouteH = 228.0f;
constexpr float kFooterY = 684.0f;

inline NVGcolor rgb(unsigned v) {
    return nvgRGB(static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 8),
                  static_cast<unsigned char>(v));
}
inline NVGcolor rgba(unsigned v, float a) {
    auto c = rgb(v);
    c.a = a;
    return c;
}

namespace col {
constexpr unsigned ink = 0x090B10;
constexpr unsigned panel = 0x121620;
constexpr unsigned well = 0x07090D;
constexpr unsigned edge = 0x1F2633;
constexpr unsigned edgeHi = 0x2D3746;
constexpr unsigned bone = 0xEAE5DA;
constexpr unsigned dust = 0xA6ACB6;
constexpr unsigned dim = 0x737C89;
constexpr unsigned accent = 0xD9A448;  // brass: audio path, values
constexpr unsigned cool = 0x78AFC4;    // modulation
constexpr unsigned alert = 0xC65C4F;

constexpr unsigned knobWell = 0x121821;
constexpr unsigned knobTop = 0x36414F;
constexpr unsigned knobBot = 0x141B24;
constexpr unsigned knobRim = 0x3E4B5A;
}  // namespace col

constexpr const char* kFontMono = "orrery-mono";
constexpr const char* kFontMonoMed = "orrery-mono-medium";
constexpr const char* kFontDisplay = "orrery-display";
constexpr const char* kFontUi = "orrery-ui";
constexpr const char* kFontUiSemi = "orrery-ui-semibold";

}  // namespace orrery::gui
