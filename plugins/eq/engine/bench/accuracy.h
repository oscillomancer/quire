#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include "../src/design.h"
namespace supple::eq::measurement {
// Independent analog transfer functions, evaluated on the imaginary axis.
inline double analogDb(int type, double f, double gain, double q, double hz) {
    const auto a = std::pow(10., gain / 40.);
    const std::complex<double> s(0, hz / f);
    std::complex<double> h;
    if (type == Bell)
        h = (s * s + a * s / q + 1.) / (s * s + s / (q * a) + 1.);
    else if (type == LowShelf) {
        const auto z = s * std::sqrt(a);
        h = (z * z + a * z / q + a * a) / (z * z + z / q + 1.);
    } else {
        const auto z = s / std::sqrt(a);
        h = (a * a * z * z + a * z / q + 1.) / (z * z + z / q + 1.);
    }
    return 20 * std::log10(std::abs(h));
}
struct Error {
    double db = 0, f0 = 0, gain = 0, q = 0, hz = 0;
    int type = Bell;
};
inline void measureCase(Error& worst, int type, double f, double gain, double q, double fs,
                        int points) {
    const auto d = design(type, f, gain, q, 3, fs, true);
    auto probe = [&](double hz) {
        if (hz < 20 || hz > 20000) return;
        const auto magnitude = designMagnitude(d, hz, fs);
        const auto error = std::abs(20 * std::log10(magnitude) - analogDb(type, f, gain, q, hz));
        if (!std::isfinite(error)) {
            worst.db = std::numeric_limits<double>::infinity();
        } else if (error > worst.db)
            worst = {error, f, gain, q, hz, type};
    };
    for (int k = 0; k <= points; ++k) {
        probe(20 * std::pow(1000., static_cast<double>(k) / points));
        probe(20 + 19980. * k / points);
    }
    const auto a = std::pow(10., gain / 40.);
    const double centre =
        type == Bell ? f : f * (type == LowShelf ? 1 / std::sqrt(a) : std::sqrt(a));
    for (double offset : {-2., -1., -.5, 0., .5, 1., 2.})
        probe(centre * (1 + offset / (q * std::max(a, 1 / a))));
    probe(f);
}
inline Error ordinary(int type, double fs, int points = 512) {
    Error worst;
    constexpr double qs[] = {.1, .2, .3, .4, .5, .6, .71, .8, 1., 1.5, 2., 3., 4.};
    for (int k = 0; k <= 24; ++k) {
        const double f = k < 20 ? 10 * std::pow(1000., k / 19.) : 8000 + 500. * (k - 20);
        for (int gain = -30; gain <= 30; gain += 3) {
            if (gain == 0) continue;
            for (double q : qs) {
                if (type != Bell && q > 1) continue;
                measureCase(worst, type, f, gain, q, fs, points);
            }
        }
    }
    return worst;
}
inline Error extreme(int type, double fs, bool nearNyquist, int points = 512) {
    Error worst;
    for (double f : nearNyquist ? std::array<double, 4>{10000, 16000, .45 * fs, .49 * fs}
                                : std::array<double, 4>{1000, 4000, 8000, 10000})
        for (double gain : {-30., -12., -6., 6., 12., 30.})
            for (double q : {1.2, 2., 4., 8., 16., 24.}) {
                if (type == Bell && nearNyquist && q < 4) continue;
                measureCase(worst, type, f, gain, q, fs, points);
            }
    return worst;
}
}  // namespace supple::eq::measurement
