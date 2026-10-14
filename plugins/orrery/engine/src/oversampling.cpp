// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "oversampling.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <numbers>

namespace supple::orrery {

std::vector<double> designHalfband(int count, double transition) {
    assert(count >= 1 && count <= Halfband::kMaxCoefs);
    assert(transition > 0.0 && transition < 0.5);
    const double pi = std::numbers::pi;
    const int order = 2 * count + 1;
    // Selectivity of the half-band elliptic filter: pass edge ωp = π/2 − π·tb,
    // stop edge π − ωp, so k = tan(ωp/2) / tan(ωs/2) = tan²(ωp/2).
    const double t = std::tan(pi / 4.0 - pi * transition / 2.0);
    const double k = t * t;
    // Nome from the complementary modulus (A&S 17.3.21).
    const double kp = std::sqrt(std::sqrt(1.0 - k * k));  // sqrt(k')
    const double e = 0.5 * (1.0 - kp) / (1.0 + kp);
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    std::vector<double> a;
    for (int i = 1; i <= count; ++i) {
        // Theta-series evaluation of the elliptic sine at the i-th zero.
        double num = 0.0, den = 0.0;
        for (int m = 0; m < 64; ++m) {
            const double term = std::pow(q, m * (m + 1)) * std::sin((2 * m + 1) * i * pi / order) *
                                (m % 2 ? -1.0 : 1.0);
            num += term;
            if (std::fabs(term) < 1e-30) break;
        }
        for (int m = 1; m < 64; ++m) {
            const double term =
                std::pow(q, m * m) * std::cos(2.0 * m * i * pi / order) * (m % 2 ? -1.0 : 1.0);
            den += term;
            if (std::fabs(term) < 1e-30) break;
        }
        const double w = 2.0 * std::pow(q, 0.25) * num / (1.0 + 2.0 * den);
        const double w2 = w * w;
        const double x = std::sqrt((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
        a.push_back((1.0 - x) / (1.0 + x));
    }
    std::sort(a.begin(), a.end());
    return a;
}

double halfbandResponseDb(const std::vector<double>& coefs, double f) {
    using C = std::complex<double>;
    const C z1 = std::polar(1.0, -2.0 * std::numbers::pi * f);  // z⁻¹
    const C z2 = z1 * z1;
    C a0 = 1.0, a1 = 1.0;
    for (std::size_t i = 0; i < coefs.size(); ++i) {
        const C sec = (coefs[i] + z2) / (1.0 + coefs[i] * z2);
        (i % 2 == 0 ? a0 : a1) *= sec;
    }
    const C h = 0.5 * (a0 + z1 * a1);
    return 20.0 * std::log10(std::max(std::abs(h), 1e-300));
}

void Halfband::setCoefs(const std::vector<double>& coefs) {
    assert(coefs.size() <= static_cast<std::size_t>(kMaxCoefs));
    n0_ = n1_ = 0;
    std::array<double, kMaxCoefs> b0{}, b1{};
    for (std::size_t i = 0; i < coefs.size(); ++i) {
        if (i % 2 == 0)
            b0[static_cast<std::size_t>(n0_++)] = coefs[i];
        else
            b1[static_cast<std::size_t>(n1_++)] = coefs[i];
    }
    for (int i = 0; i < n0_; ++i)
        s_[static_cast<std::size_t>(i)].a = b0[static_cast<std::size_t>(i)];
    for (int i = 0; i < n1_; ++i)
        s_[static_cast<std::size_t>(n0_ + i)].a = b1[static_cast<std::size_t>(i)];
    reset();
}

void Halfband::reset() {
    for (auto& s : s_) s.x1 = s.y1 = 0.0;
    oddPrev_ = 0.0;
}

double Halfband::process(double even, double odd) {
    // At the low rate each z⁻² section is y = a·(x − y[n−1]) + x[n−1].
    double y0 = even;
    for (int i = 0; i < n0_; ++i) {
        Section& s = s_[static_cast<std::size_t>(i)];
        const double y = s.a * (y0 - s.y1) + s.x1;
        s.x1 = y0;
        s.y1 = y;
        y0 = y;
    }
    // Branch 1 sees the odd samples one input sample later (z⁻¹): the previous odd.
    double y1 = oddPrev_;
    oddPrev_ = odd;
    for (int i = 0; i < n1_; ++i) {
        Section& s = s_[static_cast<std::size_t>(n0_ + i)];
        const double y = s.a * (y1 - s.y1) + s.x1;
        s.x1 = y1;
        s.y1 = y;
        y1 = y;
    }
    return 0.5 * (y0 + y1);
}

void Decimator::setFactor(int factor) {
    assert(factor == 1 || factor == 2 || factor == 4);
    factor_ = factor;
    // Final 2× stage: pass 0..20 kHz, stop from 28 kHz at a 96 kHz input
    // (tb = 8/96). A first 4× stage only has to protect 0..20 kHz of the
    // final band (tb ≈ 0.29 at 192 kHz), so it needs few sections.
    static const std::vector<double> kFinal = designHalfband(6, 8.0 / 96.0);
    static const std::vector<double> kFirst = designHalfband(3, 0.29);
    if (factor == 2) {
        first_.setCoefs(kFinal);
    } else if (factor == 4) {
        first_.setCoefs(kFirst);
        second_.setCoefs(kFinal);
    }
    reset();
}

void Decimator::reset() {
    first_.reset();
    second_.reset();
}

void Decimator::process(const float* in, float* out, std::uint32_t n) {
    switch (factor_) {
        case 1:
            std::copy_n(in, n, out);
            break;
        case 2:
            for (std::uint32_t i = 0; i < n; ++i)
                out[i] = static_cast<float>(first_.process(static_cast<double>(in[2 * i]),
                                                           static_cast<double>(in[2 * i + 1])));
            break;
        case 4:
            for (std::uint32_t i = 0; i < n; ++i) {
                const double a = first_.process(static_cast<double>(in[4 * i]),
                                                static_cast<double>(in[4 * i + 1]));
                const double b = first_.process(static_cast<double>(in[4 * i + 2]),
                                                static_cast<double>(in[4 * i + 3]));
                out[i] = static_cast<float>(second_.process(a, b));
            }
            break;
        default:
            std::fill_n(out, n, 0.0f);
    }
}

}  // namespace supple::orrery
