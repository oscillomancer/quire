// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Waveshapers with antiderivative anti-aliasing (ADAA, Parker, Zavalishin
// and Le Bihan, DAFx 2016): instead of the shape at each sample, the
// shape's average over the segment from the previous sample to this one,
// (F(x) - F(x[-1])) / (x - x[-1]) with F its antiderivative. Much of the
// aliasing a clipper makes goes away at the cost of a few operations, and it
// stacks with oversampling: ADAA at 2x often beats plain clipping at 8x.
//
//   dsp::Adaa<dsp::TanhShape>::State state;     // one per channel
//   y = dsp::Adaa<dsp::TanhShape>::tick(state, x * drive);
//
// The average delays the signal by half a sample, and a sharp corner is
// slightly softened, both as a gentle low-pass would.
#pragma once

#include <algorithm>
#include <cmath>

namespace supple::dsp {

// A shape is f (the curve) and F (its antiderivative).
struct HardClipShape {
    static double f(double x) { return std::clamp(x, -1.0, 1.0); }
    static double F(double x) {
        const double a = std::fabs(x);
        return a <= 1.0 ? 0.5 * x * x : a - 0.5;
    }
};
struct TanhShape {
    static double f(double x) { return std::tanh(x); }
    // log(cosh(x)), without overflow for large |x|.
    static double F(double x) {
        const double a = std::fabs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - 0.6931471805599453;
    }
};
// The cubic soft clip: x - x^3 / 3 inside [-1, 1], +-2/3 outside.
struct CubicShape {
    static double f(double x) {
        if (x >= 1.0) return 2.0 / 3.0;
        if (x <= -1.0) return -2.0 / 3.0;
        return x - x * x * x / 3.0;
    }
    static double F(double x) {
        const double a = std::fabs(x);
        return a <= 1.0 ? 0.5 * x * x - x * x * x * x / 12.0 : 2.0 / 3.0 * a - 0.25;
    }
};

template <class Shape>
struct Adaa {
    struct State {
        double x1 = 0.0;
        double F1 = Shape::F(0.0);
    };
    static double tick(State& s, double x) {
        const double F = Shape::F(x);
        const double dx = x - s.x1;
        // Close together, the quotient is ill-conditioned; the shape at the
        // midpoint is its limit.
        const double y = std::fabs(dx) < 1e-6 ? Shape::f(0.5 * (x + s.x1)) : (F - s.F1) / dx;
        s.x1 = x;
        s.F1 = F;
        return y;
    }
};

}  // namespace supple::dsp
