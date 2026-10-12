// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// This product's palette. The automatic editor takes its look from it too.
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x0E0F12,
    /* surface    */ 0x181A1F,
    /* raised     */ 0x23262D,
    /* recess     */ 0x08090B,
    /* line       */ 0x292C34,
    /* lineLit    */ 0x3A3E48,
    /* textFaint  */ 0x636874,
    /* textMuted  */ 0x9599A4,
    /* text       */ 0xECE9E4,
    /* accent     */ 0xE8584A,
    /* accent2    */ 0xE8A44A,
    /* modulation */ 0x9D8DF1,
    /* alert      */ 0xF0C04A,
    /* knobTrack  */ 0x0C0D10,
    /* knobTop    */ 0x3F434D,
    /* knobBottom */ 0x1B1D22,
    /* knobRim    */ 0x4B505B,
    /* radius     */ 7.0f,
};

}  // namespace product
