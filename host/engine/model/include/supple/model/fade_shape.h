// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Shared item fade gain: display and playback use the same shape and bend.
#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

#include "supple/model/objects.h"

namespace supple::model {

inline double fadeGain(FadeShape shape, double curve, double t) {
    t = std::clamp(t, 0.0, 1.0);
    double gain = t;
    switch (shape) {
        case FadeShape::Linear:
            break;
        case FadeShape::Slow:
            gain = t * t;
            break;
        case FadeShape::Fast:
            gain = 1.0 - (1.0 - t) * (1.0 - t);
            break;
        case FadeShape::SlowFast:
            gain = t * t * (3.0 - 2.0 * t);
            break;
        case FadeShape::FastSlow:
            gain = 0.5 - std::sin(std::asin(1.0 - 2.0 * t) / 3.0);
            break;
        case FadeShape::Cos:
            gain = std::sin(t * std::numbers::pi / 2.0);
            break;
        case FadeShape::Exp:
            gain = t * t * t;
            break;
    }
    if (curve == 0) return gain;
    return std::pow(std::clamp(gain, 0.0, 1.0), std::exp2(std::clamp(curve, -100.0, 100.0) / 50.0));
}

}  // namespace supple::model
