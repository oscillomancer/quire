#pragma once
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include "eq/processor.h"
namespace supple::eq {
inline int naturalStages(int type, int slope, double frequency, double gain, double q, double fs) {
    if (type != Bell && type != LowShelf && type != HighShelf && type != Tilt) gain = 0;
    if (type == Tilt) q = .71;
    if ((type == HighPass || type == LowPass) && slope != 1) {
        constexpr int orders[] = {1, 2, 3, 4, 6, 8, 12, 16};
        const int order = orders[slope];
        q = order == 1 ? .71 : 1 / (2 * std::sin(std::numbers::pi / (2 * order)));
    }
    const double effectiveQ = q * std::pow(10., std::abs(gain) / 40.);
    const double difficulty = effectiveQ * std::pow(std::min(frequency, fs * .49) / fs, 2);
    int stages = 4;
    double capacity = .25;
    while (stages < 7 && difficulty > capacity) {
        ++stages;
        capacity *= 4;
    }
    return stages;
}
struct Section {
    double g = 0, k = 0, m0 = 1, m1 = 0, m2 = 0, a1 = 1, a2 = 0, a3 = 0;
    bool first = false;
    void finish() {
        a1 = 1 / (1 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    std::complex<double> response(double hz, double fs) const {
        const std::complex<double> t(0, std::tan(std::numbers::pi * hz / fs));
        if (first) return m0 + m2 * g / (t + g);
        const auto den = t * t + k * g * t + g * g;
        return m0 + (m1 * g * t + m2 * g * g) / den;
    }
};
struct Design {
    std::array<Section, 8> s{};
    int n = 0;
};
inline Design design(int type, double freq, double gain, double q, int slope, double fs,
                     bool natural) {
    Design d;
    const double g =
        natural ? std::numbers::pi * freq / fs : std::tan(std::numbers::pi * freq / fs);
    const double a = std::pow(10., gain / 40.);
    auto add = [&](Section s) {
        s.finish();
        d.s[static_cast<std::size_t>(d.n++)] = s;
    };
    if ((type == Bell || type == LowShelf || type == HighShelf || type == Tilt) && gain == 0)
        return d;
    switch (type) {
        case Bell:
            add({g, 1 / (q * a), 1, (a * a - 1) / (q * a), 0});
            break;
        case LowShelf:
            add({g / std::sqrt(a), 1 / q, 1, (a - 1) / q, a * a - 1});
            break;
        case HighShelf:
            add({g * std::sqrt(a), 1 / q, a * a, (1 - a) * a / q, 1 - a * a});
            break;
        case Notch:
            add({g, 1 / q, 1, -1 / q, 0});
            break;
        case BandPass:
            add({g, 1 / q, 0, 1 / q, 0});
            break;
        case Tilt: {
            const double low = std::pow(10., -gain / 40.), high = 1 / low;
            Section l{g / std::sqrt(low), 0, 1, 0, low - 1};
            l.first = true;
            add(l);
            Section h{g * std::sqrt(high), 0, high, 0, 1 - high};
            h.first = true;
            add(h);
            break;
        }
        default: {
            constexpr int orders[] = {1, 2, 3, 4, 6, 8, 12, 16};
            const int order = orders[slope];
            if (order % 2) {
                Section s{g, 0, type == HighPass ? 1. : 0., 0, type == HighPass ? -1. : 1.};
                s.first = true;
                add(s);
            }
            for (int i = 0; i < order / 2; ++i) {
                const double qq =
                    order == 2 ? q
                               : 1 / (2 * std::sin((2 * i + 1) * std::numbers::pi / (2 * order)));
                add({g, 1 / qq, type == HighPass ? 1. : 0., type == HighPass ? -1 / qq : 0.,
                     type == HighPass ? -1. : 1.});
            }
        }
    }
    return d;
}
inline double designMagnitude(const Design& d, double hz, double fs) {
    double m = 1;
    for (int i = 0; i < d.n; ++i) m *= std::abs(d.s[static_cast<std::size_t>(i)].response(hz, fs));
    return m;
}
}  // namespace supple::eq
