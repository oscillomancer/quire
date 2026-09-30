// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#pragma once

#include <nanovg.h>

namespace gate::gui {

constexpr float kDesignW = 820.0f;
constexpr float kDesignH = 560.0f;
constexpr float kHeaderH = 62.0f;
constexpr float kMeterTop = 98.0f;
constexpr float kMeterH = 214.0f;
constexpr float kControlsTop = 350.0f;
constexpr float kFooterTop = 545.0f;
constexpr float kMargin = 30.0f;
constexpr int kHistoryColumns = 200;
constexpr float kHistorySeconds = 4.0f;

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

constexpr const char* kFontMono = "gate-mono";
constexpr const char* kFontMonoMed = "gate-mono-medium";
constexpr const char* kFontDisplay = "gate-display";
constexpr const char* kFontUi = "gate-ui";
constexpr const char* kFontUiSemi = "gate-ui-semibold";

}  // namespace gate::gui
