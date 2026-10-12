// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Windowed-sinc FIR design: the Kaiser window, and low-pass kernels built
// from it. The oversampler's half-band filters, the true-peak meter's
// interpolator and the file resampler all come from here. Designing
// allocates (the result is a vector): do it in prepare() or off the audio
// thread.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "dsp/Math.h"

namespace supple::dsp {

// The zeroth-order modified Bessel function of the first kind, by its
// series; converges quickly for the arguments a Kaiser window uses.
inline double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// Kaiser's beta for a stopband `attenuationDb` down.
inline double kaiserBeta(double attenuationDb) {
    if (attenuationDb > 50.0) return 0.1102 * (attenuationDb - 8.7);
    if (attenuationDb >= 21.0) return 0.5842 * std::pow(attenuationDb - 21.0, 0.4) + 0.07886 * (attenuationDb - 21.0);
    return 0.0;
}

// The window's value at position t in [-1, 1].
inline double kaiser(double t, double beta) {
    const double u = 1.0 - t * t;
    return u < 0.0 ? 0.0 : besselI0(beta * std::sqrt(u)) / besselI0(beta);
}

inline double sinc(double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// A linear-phase low-pass of `taps` taps (odd keeps the centre on a sample),
// cutting off at `cutoff` as a fraction of the sample rate (0.25 is half of
// Nyquist), with unity gain at DC.
inline std::vector<double> lowpassKernel(int taps, double cutoff, double attenuationDb = 100.0) {
    taps = std::max(1, taps);
    std::vector<double> h(static_cast<size_t>(taps));
    const double beta = kaiserBeta(attenuationDb), centre = 0.5 * (taps - 1);
    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        const double t = taps > 1 ? (n - centre) / centre : 0.0;
        h[static_cast<size_t>(n)] = 2.0 * cutoff * sinc(2.0 * cutoff * (n - centre)) * kaiser(t, beta);
        sum += h[static_cast<size_t>(n)];
    }
    for (double& v : h) v /= sum;
    return h;
}

// How many taps a Kaiser design needs for this attenuation over a transition
// band `width` wide (a fraction of the sample rate).
inline int kaiserTaps(double attenuationDb, double width) {
    return std::max(3, static_cast<int>(std::ceil((attenuationDb - 7.95) / (14.36 * width))) + 1);
}

}  // namespace supple::dsp
