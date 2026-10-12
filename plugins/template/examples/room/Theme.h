// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// This product's palette (see framework/gui/Theme.h for what each token is).
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x0E0D10,
    /* surface    */ 0x18161C,
    /* raised     */ 0x231F28,
    /* recess     */ 0x08070A,
    /* line       */ 0x2C2733,
    /* lineLit    */ 0x443C4E,
    /* textFaint  */ 0x6A6275,
    /* textMuted  */ 0x9C93A8,
    /* text       */ 0xEAE6EE,
    /* accent     */ 0xB48CF0,
    /* accent2    */ 0xE7B37A,
    /* modulation */ 0x7FD3C4,
    /* alert      */ 0xE0584A,
    /* knobTrack  */ 0x0F0D12,
    /* knobTop    */ 0x3F3848,
    /* knobBottom */ 0x1D1A22,
    /* knobRim    */ 0x51485E,
    /* radius     */ 7.0f,
};

}  // namespace product
