// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The master playrate knob's travel (gui.md §4.1): log-scaled around 1.0 at
// the middle, out to the chosen range's ends. The transport bar's knob and the
// "Transport: playrate (MIDI CC)" action share it.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace supple::gui {

struct RateRange {
    const char* name;
    double low, high;
};
inline constexpr std::array<RateRange, 4> kRateRanges{{{"0.25-4.0", 0.25, 4.0},
                                                       {"0.5-2.0", 0.5, 2.0},
                                                       {"0.75-1.5", 0.75, 1.5},
                                                       {"0.9-1.1", 0.9, 1.1}}};

inline double ratePosition(double rate, std::size_t range) {
    const RateRange& r = kRateRanges[std::min(range, kRateRanges.size() - 1)];
    const double n = rate <= 1 ? 0.5 + 0.5 * std::log(rate) / -std::log(r.low)
                               : 0.5 + 0.5 * std::log(rate) / std::log(r.high);
    return std::clamp(n, 0.0, 1.0);
}

inline double positionRate(double position, std::size_t range) {
    const RateRange& r = kRateRanges[std::min(range, kRateRanges.size() - 1)];
    return std::exp(position <= 0.5 ? std::log(r.low) * (1 - 2 * position)
                                    : std::log(r.high) * (2 * position - 1));
}

}  // namespace supple::gui
