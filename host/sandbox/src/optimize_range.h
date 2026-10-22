// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The search window for a parameter the caller left unconstrained: the current
// value ± a musically sized step, clamped to the parameter's own range.
//
// A reference search starts from the agent's structure, which is already close.
// Searching each parameter's whole range spreads the population over values
// that cannot be right (the frequency range is ten octaves, so one σ = 0.2 of
// it is a factor of ~8), and the run stagnates without improving on the start
// (docs/benchmarks/optimizer/2026-10-01-tuning.md). An agent that knows the
// answer is far outside the window can still name the key with min/max, which
// keeps the full range.
#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include "supple/params/params.h"

namespace supple::sandbox::detail {

// Frequency, time and pitch-interval quantities are moved by a factor — a
// doubling is an octave — so their window is a ratio; everything else is a
// share of the parameter's range.
inline bool ratioPerceived(params::Unit unit) {
    return unit == params::Unit::Hz || unit == params::Unit::ms || unit == params::Unit::st ||
           unit == params::Unit::ct || unit == params::Unit::oct;
}

// Whether "auto" leaves a key out: performance and placement parameters
// (voice.*, pans, envelope curves, delays and holds, velocity sensitivity,
// LFO fades) and the output level, as `output` or `output.level` (loudness is
// matched, not optimized).
inline bool autoExcluded(std::string_view k) {
    auto ends = [&](std::string_view suffix) {
        return k.size() >= suffix.size() && k.substr(k.size() - suffix.size()) == suffix;
    };
    return k.starts_with("voice.") || k == "output" || k == "output.level" || ends(".pan") ||
           ends("_curve") || ends(".delay") || ends(".hold") || ends(".velocity") || ends(".fade");
}

inline constexpr double kRatioWindow = 4.0;   // ±2 octaves (×4 / ÷4)
inline constexpr double kShareWindow = 0.25;  // ±25 % of the range

// {lo, hi} in the normalized domain, around `current`, clamped to [0, 1].
inline std::pair<double, double> autoWindow(const params::ParamSpec& spec, double current) {
    const double u0 = params::toNormalized(spec, current);
    double half = kShareWindow;
    if (ratioPerceived(spec.unit) && current > 0.0) {
        const double lo = params::toNormalized(spec, std::max(spec.min, current / kRatioWindow));
        const double hi = params::toNormalized(spec, std::min(spec.max, current * kRatioWindow));
        // A ratio window is asymmetric in the normalized domain on a pow curve
        // (v = min + (max − min)·n^k): take the wider side so both ×4 and ÷4 fit.
        half = std::max(u0 - lo, hi - u0);
    }
    return {std::clamp(u0 - half, 0.0, 1.0), std::clamp(u0 + half, 0.0, 1.0)};
}

}  // namespace supple::sandbox::detail
