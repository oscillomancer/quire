// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// This product's palette. Rewrite it for every product: the widgets take
// their whole look from these tokens (see framework/gui/Theme.h).
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x0B0D11,
    /* surface    */ 0x161A21,
    /* raised     */ 0x20252E,
    /* recess     */ 0x07090C,
    /* line       */ 0x252C37,
    /* lineLit    */ 0x343D4A,
    /* textFaint  */ 0x5D6674,
    /* textMuted  */ 0x8F98A5,
    /* text       */ 0xE8E4DC,
    /* accent     */ 0xE8944A,
    /* accent2    */ 0x6FB3A8,
    /* modulation */ 0x9D8DF1,
    /* alert      */ 0xE0584A,
    /* knobTrack  */ 0x0D1015,
    /* knobTop    */ 0x3B4553,
    /* knobBottom */ 0x181D25,
    /* knobRim    */ 0x48535F,
    /* radius     */ 7.0f,
};

}  // namespace product
