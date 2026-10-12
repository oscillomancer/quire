// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// This product's palette (see framework/gui/Theme.h for what each token is).
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x0C0F10,
    /* surface    */ 0x151A1C,
    /* raised     */ 0x1F2629,
    /* recess     */ 0x070909,
    /* line       */ 0x263033,
    /* lineLit    */ 0x3A484C,
    /* textFaint  */ 0x5E6C70,
    /* textMuted  */ 0x8FA0A5,
    /* text       */ 0xE3EBED,
    /* accent     */ 0x4FC3C8,
    /* accent2    */ 0x8FD9A8,
    /* modulation */ 0xC79BF2,
    /* alert      */ 0xE0584A,
    /* knobTrack  */ 0x0E1213,
    /* knobTop    */ 0x3A4548,
    /* knobBottom */ 0x1A2022,
    /* knobRim    */ 0x4A585C,
    /* radius     */ 7.0f,
};

}  // namespace product
