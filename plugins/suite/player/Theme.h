// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The player's palette, which the automatic editor of every script takes.
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x0D0F12,
    /* surface    */ 0x171A1F,
    /* raised     */ 0x22262D,
    /* recess     */ 0x08090B,
    /* line       */ 0x282C34,
    /* lineLit    */ 0x39404A,
    /* textFaint  */ 0x626874,
    /* textMuted  */ 0x949AA5,
    /* text       */ 0xEAE8E3,
    /* accent     */ 0x6FB3A8,
    /* accent2    */ 0xE8944A,
    /* modulation */ 0x9D8DF1,
    /* alert      */ 0xE0584A,
    /* knobTrack  */ 0x0C0D10,
    /* knobTop    */ 0x3E434D,
    /* knobBottom */ 0x1A1D22,
    /* knobRim    */ 0x4A505B,
    /* radius     */ 7.0f,
};

}  // namespace product
