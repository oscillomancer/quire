// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// How an envelope segment moves from its point to the next: t in 0..1 maps to
// the fraction of the value change. Engine playback and the GUI's automation
// lanes share it, so what is drawn is what plays.
#pragma once

#include <algorithm>

#include "supple/model/objects.h"

namespace supple::model {

inline double shapeCurve(PointShape shape, double tension, double t) {
    switch (shape) {
        case PointShape::Linear:
            return t;
        case PointShape::Square:
            return 0.0;  // holds until the next point
        case PointShape::Slow:
            return t * t * (3.0 - 2.0 * t);
        case PointShape::FastStart:
            return 1.0 - (1.0 - t) * (1.0 - t);
        case PointShape::FastEnd:
            return t * t;
        case PointShape::Bezier:
            return t + std::clamp(tension, -1.0, 1.0) * t * (1.0 - t);
    }
    return t;
}

}  // namespace supple::model
