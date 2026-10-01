// SPDX-License-Identifier: MIT
// Independent offline measurements: no engine filter coefficients used here.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>
#include "reverb/processor.h"
namespace measurements {
using Stereo = std::array<std::vector<double>, 2>;
inline Stereo impulse(supple::reverb::Values v, double fs, double seconds, int block = 512) {
    supple::reverb::Processor p;
    for (std::size_t i = 0; i < v.size(); ++i) p.setParam(i, v[i]);
    if (!p.prepare(fs, 4096)) return {};
    const auto total = static_cast<std::size_t>(std::ceil(seconds * fs));
    Stereo y{std::vector<double>(total), std::vector<double>(total)};
    std::vector<double> input(total);
    input[0] = 1;
    for (std::size_t off = 0; off < total;) {
        const auto n =
            static_cast<std::uint32_t>(std::min(total - off, static_cast<std::size_t>(block)));
        const double* in[] = {input.data() + off, input.data() + off};
        double* out[] = {y[0].data() + off, y[1].data() + off};
        p.process(in, out, n);
        off += n;
    }
    return y;
}
struct Biquad {
    double b0, b1, b2, a1, a2, s1 = 0, s2 = 0;
    double run(double x) {
        const double y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }
};
inline Biquad pass(double f, double fs, double q, bool high) {
    const double w = 2 * std::numbers::pi * f / fs, c = std::cos(w), alpha = std::sin(w) / (2 * q),
                 a0 = 1 + alpha;
    const double b = (high ? 1 + c : 1 - c) * .5;
    return {b / a0, (high ? -2 : 2) * b / a0, b / a0, -2 * c / a0, (1 - alpha) / a0};
}
inline double rt60(const Stereo& input, double frequency, double fs) {
    // 8th-order octave: 4th-order Butterworth at each edge. Its own decay
    // is short relative to all test targets; stereo energies are combined.
    std::array<std::array<Biquad, 4>, 2> filters;
    for (auto& channel : filters) {
        channel = {pass(frequency / std::numbers::sqrt2, fs, .541196100146197, true),
                   pass(frequency / std::numbers::sqrt2, fs, 1.306562964876377, true),
                   pass(frequency * std::numbers::sqrt2, fs, .541196100146197, false),
                   pass(frequency * std::numbers::sqrt2, fs, 1.306562964876377, false)};
    }
    std::vector<double> energy(input[0].size());
    for (std::size_t i = 0; i < energy.size(); ++i)
        for (std::size_t c = 0; c < 2; ++c) {
            double x = input[c][i];
            for (auto& f : filters[c]) x = f.run(x);
            energy[i] += x * x;
        }
    double sum = 0;
    for (std::size_t i = energy.size(); i-- > 0;) {
        sum += energy[i];
        energy[i] = sum;
    }
    if (sum <= 0) return 0;
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::size_t i = 0; i < energy.size(); i += 8) {
        const double y = 10 * std::log10(std::max(energy[i] / sum, 1e-100));
        if (y > -5 || y < -35) continue;
        const double x = static_cast<double>(i) / fs;
        n += 1;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    if (n < 2) return 0;
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return -60 / slope;
}
inline double densityTime(const std::vector<double>& ir, double fs) {
    const auto window = static_cast<std::size_t>(std::lround(.020 * fs));
    const auto step = static_cast<std::size_t>(std::max(1., std::round(.001 * fs)));
    int consecutive = 0;
    for (std::size_t start = 0; start + window < ir.size(); start += step) {
        double mean = 0, power = 0;
        for (std::size_t j = 0; j < window; ++j) {
            const double x = ir[start + j];
            mean += x;
            power += x * x;
        }
        mean /= static_cast<double>(window);
        const double rms =
            std::sqrt(std::max(0., power / static_cast<double>(window) - mean * mean));
        std::size_t count = 0;
        if (rms > 1e-20)
            for (std::size_t j = 0; j < window; ++j)
                if (std::abs(ir[start + j] - mean) > rms) ++count;
        const double density = static_cast<double>(count) /
                               (static_cast<double>(window) * std::erfc(1 / std::numbers::sqrt2));
        if (density >= .8)
            ++consecutive;
        else
            consecutive = 0;
        // A 10 ms sustained crossing avoids declaring one isolated spike mixed.
        if (consecutive >= 10) return (static_cast<double>(start) / fs + .010 - .009) * 1000;
    }
    return 1e9;
}
inline double correlation(const Stereo& y, std::size_t first, std::size_t last) {
    double a = 0, b = 0, ab = 0;
    for (auto i = first; i < last; ++i) {
        a += y[0][i] * y[0][i];
        b += y[1][i] * y[1][i];
        ab += y[0][i] * y[1][i];
    }
    return ab / std::sqrt(a * b);
}
}  // namespace measurements
