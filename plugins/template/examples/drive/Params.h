// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter table: the only description of this product's parameters,
// and, with no editor code, of its editor too. Groups become panels, in the
// order they first appear; help() becomes each control's tooltip.
// IDs are session ABI: append, never reorder, renumber or reuse.
#pragma once

#include <cstdint>

#include "core/Param.h"

namespace product {

enum ParamId : uint32_t {
    kDrive = 0,
    kShape = 1,
    kTone = 2,
    kMix = 3,
    kOutput = 4,
    kPeak = 5,
    kBypass = 6,
    kLoudness = 7,
    kQuality = 8,
};

inline constexpr const char* kShapeLabels[] = {"Soft", "Hard", "Fold"};
// How many times over the shaper runs: 1, 2, 4, 8.
inline constexpr const char* kQualityLabels[] = {"Off", "2x", "4x", "8x"};

inline constexpr supple::ParamDef kParams[] = {
    supple::decibels(kDrive, "Drive", 0.0, 36.0, 12.0).in("Drive").modulatable().smoothed(20.0)
        .help("Gain into the shaper: the more, the more it saturates."),
    supple::choice(kShape, "Shape", kShapeLabels, 0).in("Drive")
        .help("Soft rounds the peaks off, Hard clips them flat, Fold reflects them back."),
    supple::choice(kQuality, "Quality", kQualityLabels, 2).in("Drive").restartOnChange()
        .help("Oversampling: the shaper runs at this many times the rate, so what it adds above "
              "hearing is filtered away instead of folding back down as harsh, unrelated tones. "
              "Costs CPU and a little latency."),
    supple::frequency(kTone, "Tone", 500.0, 20000.0, 20000.0).in("Tone").modulatable().smoothed(20.0)
        .help("A gentle low-pass after the shaper, to take the edge off."),
    supple::percent(kMix, "Mix", 100.0).in("Output").modulatable().smoothed(20.0)
        .help("How much of the saturated signal is heard; the rest is dry."),
    supple::decibels(kOutput, "Output", -24.0, 12.0, -6.0).in("Output").modulatable().smoothed(20.0)
        .help("The level out, after the mix."),
    supple::decibels(kPeak, "Peak", -60.0, 6.0, -60.0).unitText("dBTP").in("Output").output()
        .help("The loudest point out in the last moment, between samples too (true peak)."),
    supple::decibels(kLoudness, "Loudness", -60.0, 0.0, -60.0).unitText("LUFS").in("Output").output()
        .help("Short-term loudness out: the last three seconds, weighted as hearing is, as broadcast meters read it."),
    supple::toggle(kBypass, "Bypass", false).bypass(),
};

}  // namespace product
