// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// This product's palette (see framework/gui/Theme.h for what each token is).
#pragma once

#include "gui/Theme.h"

namespace product {

inline constexpr supple::gui::Theme kTheme = {
    /* ground     */ 0x0F0D12,
    /* surface    */ 0x19161E,
    /* raised     */ 0x2A2531,
    /* recess     */ 0x08070A,
    /* line       */ 0x2C2733,
    /* lineLit    */ 0x443C4E,
    /* textFaint  */ 0x6A6276,
    /* textMuted  */ 0x9E95AB,
    /* text       */ 0xEAE6EF,
    /* accent     */ 0xB78CF0,
    /* accent2    */ 0x7FC4E8,
    /* modulation */ 0xF0A35E,
    /* alert      */ 0xE0584A,
    /* knobTrack  */ 0x110F14,
    /* knobTop    */ 0x433C4C,
    /* knobBottom */ 0x1E1B23,
    /* knobRim    */ 0x544B5F,
    /* radius     */ 7.0f,
};

}  // namespace product
