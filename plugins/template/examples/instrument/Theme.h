// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// This product's palette (see framework/gui/Theme.h for what each token is).
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x0C0E12,
    /* surface    */ 0x171B22,
    /* raised     */ 0x21262F,
    /* recess     */ 0x080A0D,
    /* line       */ 0x262D38,
    /* lineLit    */ 0x35404D,
    /* textFaint  */ 0x5E6775,
    /* textMuted  */ 0x909AA7,
    /* text       */ 0xE6E8EC,
    /* accent     */ 0x6FB7E8,
    /* accent2    */ 0x8FD1B4,
    /* modulation */ 0xC79BF2,
    /* alert      */ 0xE0584A,
    /* knobTrack  */ 0x0D1116,
    /* knobTop    */ 0x3A4654,
    /* knobBottom */ 0x171D26,
    /* knobRim    */ 0x475463,
    /* radius     */ 7.0f,
};

}  // namespace product
