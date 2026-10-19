// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The palette of every plugin made from a script, which its automatic
// editor takes: the player's, a source export's and the stock plugins' look
// alike. Warm and earthy, apart from the host's graphite: walnut panels,
// a soil-dark floor for graphs, parchment text, clay and sage.
#pragma once

#include "gui/Theme.h"

namespace supple::suite {

inline constexpr gui::Theme kScriptTheme = {
    /* ground     */ 0x251D17,  // walnut
    /* surface    */ 0x30271F,
    /* raised     */ 0x3E3229,
    /* recess     */ 0x171210,  // soil
    /* line       */ 0x43372D,
    /* lineLit    */ 0x5E4E41,
    /* textFaint  */ 0x8A7B6C,
    /* textMuted  */ 0xBBAA96,
    /* text       */ 0xF4ECDF,  // parchment
    /* accent     */ 0xD97757,  // clay
    /* accent2    */ 0xA3B27C,  // sage
    /* modulation */ 0xB59BCB,  // heather
    /* alert      */ 0xE5553F,
    /* knobTrack  */ 0x14100D,
    /* knobTop    */ 0x4D4036,
    /* knobBottom */ 0x2A221C,
    /* knobRim    */ 0x605043,
    /* radius     */ 7.0f,
};

}  // namespace supple::suite
