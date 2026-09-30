// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Orrery's palette, type and panel geometry. The token names are a contract
// with the shared Widgets.h (see CONVENTIONS.md); the layout constants belong
// to this product's editor.
#pragma once

#include <nanovg.h>

namespace supple::transient::gui {

constexpr float kDesignW = 900, kDesignH = 560, kHeaderH = 62, kFooterTop = 542;
static_assert(kDesignW <= 900 && kDesignH <= 560);
constexpr int kHistoryColumns = 540;
constexpr float kHistorySeconds = 2;

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

constexpr const char* kFontMono = "transient-mono";
constexpr const char* kFontMonoMed = "transient-mono-medium";
constexpr const char* kFontDisplay = "transient-display";
constexpr const char* kFontUi = "transient-ui";
constexpr const char* kFontUiSemi = "transient-ui-semibold";

}  // namespace supple::transient::gui
