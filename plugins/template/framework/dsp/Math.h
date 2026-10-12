// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The constants and conversions everything in the DSP library uses. The
// library (framework/dsp) depends on nothing of the product's: any code may
// include it.
#pragma once

#include <algorithm>
#include <cmath>

namespace supple::dsp {

inline constexpr double kPi = 3.14159265358979323846;

inline double dbToGain(double db) { return std::pow(10.0, db * 0.05); }
inline double gainToDb(double gain, double floorDb = -120.0) {
    return gain > 0.0 ? std::max(20.0 * std::log10(gain), floorDb) : floorDb;
}

}  // namespace supple::dsp
