// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace supple::eq {
// Original implementation of magnitude matching in Vicanek's phi basis.
// See https://vicanek.de/articles/BiquadFits.pdf (2016), equations 24–29, 44–45.
// A small minimum-phase correction SOS fits the remaining smooth HF residual.
using FitReal = long double;
struct Matched {
    FitReal b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void invert() {
        const auto scale = b0;
        const auto oldA1 = a1, oldA2 = a2;
        a1 = b1 / scale;
        a2 = b2 / scale;
        b0 = 1 / scale;
        b1 = oldA1 / scale;
        b2 = oldA2 / scale;
    }
    Section section() const {
        Section s;
        s.matched = true;
        s.b0 = static_cast<double>(b0);
        s.b1 = static_cast<double>(b1);
        s.b2 = static_cast<double>(b2);
        s.zA1 = static_cast<double>(a1);
        s.zA2 = static_cast<double>(a2);
        return s;
    }
};
inline Matched matchedPoles(FitReal w, FitReal damping) {
    Matched c;
    c.a2 = std::exp(-2 * damping * w);
    if (damping <= 1)
        c.a1 = -2 * std::exp(-damping * w) * std::cos(w * std::sqrt(1 - damping * damping));
    else {
        // Equivalent to exp(-damping*w)*cosh(...), without overflow at low Q.
        const auto root = std::sqrt(damping * damping - 1);
        c.a1 = -std::exp(-w / (damping + root)) - std::exp(-w * (damping + root));
    }
    return c;
}
inline std::array<FitReal, 3> phiPolynomial(FitReal b0, FitReal b1, FitReal b2) {
    return {(b0 + b1 + b2) * (b0 + b1 + b2), (b0 - b1 + b2) * (b0 - b1 + b2), -4 * b0 * b2};
}
inline FitReal phiValue(const std::array<FitReal, 3>& p, FitReal phi) {
    return p[0] * (1 - phi) + p[1] * phi + 4 * p[2] * phi * (1 - phi);
}
inline void factorNumerator(Matched& c, FitReal B0, FitReal B1, FitReal B2) {
    const auto floor = std::max(B0, B1) * 1e-24L;
    const auto dc = std::sqrt(std::max(floor, B0));
    const auto ny = std::sqrt(std::max(floor, B1));
    const auto W = (dc + ny) / 2;
    // Extreme resonances may ask for a non-positive magnitude polynomial.
    // Project onto the realizable boundary; the documented accuracy domain is interior.
    B2 = std::max(B2, -W * W * (1 - 1e-12L));
    c.b0 = (W + std::sqrt(std::max(FitReal{0}, W * W + B2))) / 2;
    c.b1 = (dc - ny) / 2;
    c.b2 = c.b0 > 0 ? -B2 / (4 * c.b0) : 0;
}
inline FitReal analogSquared(int type, FitReal x, FitReal a, FitReal q) {
    if (type == Bell) {
        const auto real = 1 - x * x;
        return (real * real + (a * x / q) * (a * x / q)) /
               (real * real + (x / (q * a)) * (x / (q * a)));
    }
    const auto z = x * std::sqrt(a);
    const auto nr = a * a - z * z, dr = 1 - z * z;
    return (nr * nr + (a * z / q) * (a * z / q)) / (dr * dr + (z / q) * (z / q));
}
inline Matched matchedBoost(int type, FitReal f, FitReal a, FitReal q, FitReal fs) {
    const auto w = 2 * std::numbers::pi_v<FitReal> * f / fs;
    auto c =
        matchedPoles(type == Bell ? w : w / std::sqrt(a), 1 / (2 * q * (type == Bell ? a : 1)));
    const auto A = phiPolynomial(1, c.a1, c.a2);
    if (type == Bell) {
        // DC, peak magnitude and stationary peak. Cuts are its exact reciprocal.
        const auto phi = std::pow(std::sin(w / 2), 2);
        const auto R1 = phiValue(A, phi) * std::pow(a, 4);
        const auto R2 = (-A[0] + A[1] + 4 * (1 - 2 * phi) * A[2]) * std::pow(a, 4);
        const auto B2 = (R1 - R2 * phi - A[0]) / (4 * phi * phi);
        const auto B1 = R2 + A[0] + 4 * (2 * phi - 1) * B2;
        factorNumerator(c, A[0], B1, B2);
    } else {
        FitReal anchor = f;
        if (q > 1) {
            // The lower stationary point of the analog low-shelf magnitude.
            const auto k = 1 / (q * q) - 2;
            const auto v = 1 + a * a;
            const auto u =
                -k * a * a / (v + std::sqrt(std::max(FitReal{0}, v * v - k * k * a * a)));
            anchor = std::min(fs * .48L, f * std::sqrt(u / a));
        }
        const auto phi = std::pow(std::sin(std::numbers::pi_v<FitReal> * anchor / fs), 2);
        const auto B0 = std::pow(a, 4) * A[0];
        const auto B1 = analogSquared(LowShelf, fs / (2 * f), a, q) * A[1];
        const auto B2 = (analogSquared(LowShelf, anchor / f, a, q) * phiValue(A, phi) -
                         B0 * (1 - phi) - B1 * phi) /
                        (4 * phi * (1 - phi));
        const auto W = (std::sqrt(B0) + std::sqrt(std::max(FitReal{0}, B1))) / 2;
        factorNumerator(c, B0, B1, B2);
        if (q > 1) {
            // A bump fit can displace or undamp the shelf's opposite extremum.
            // Reject that fit if it misses the analog zero frequency by >0.5 dB.
            const auto probe = std::min(fs * .48L, f * std::sqrt(a));
            const auto p = std::pow(std::sin(std::numbers::pi_v<FitReal> * probe / fs), 2);
            const auto ratio = phiValue(phiPolynomial(c.b0, c.b1, c.b2), p) /
                               (phiValue(A, p) * analogSquared(LowShelf, probe / f, a, q));
            if (B1 <= 0 || W * W + B2 <= 0 || ratio < .891250938L || ratio > 1.122018454L) {
                // Preserve analog zero damping instead of introducing a near-unit zero.
                // Out-of-band analog zeros must not wrap into the audible band.
                const auto zeros = w * std::sqrt(a) < std::numbers::pi_v<FitReal>
                                       ? matchedPoles(w * std::sqrt(a), 1 / (2 * q))
                                       : Matched{};
                c.b0 = a * a * (1 + c.a1 + c.a2) / (1 + zeros.a1 + zeros.a2);
                c.b1 = c.b0 * zeros.a1;
                c.b2 = c.b0 * zeros.a2;
            }
        }
    }
    return c;
}

inline constexpr std::array<FitReal, 24> kFitPhi = {
    0.00427756931309479418L, 0.0170370868554658508L, 0.0380602337443566169L, 0.0669872981077806601L,
    0.103323329854382417L,   0.146446609406726241L,  0.195619285495639644L,  0.249999999999999944L,
    0.308658283817455081L,   0.37059047744873963L,   0.434736903889974158L,  0.499999999999999889L,
    0.565263096110025787L,   0.62940952255126037L,   0.691341716182544697L,  0.749999999999999889L,
    0.804380714504360439L,   0.853553390593273731L,  0.896676670145617472L,  0.933012701892219409L,
    0.961939766255643369L,   0.982962913144534101L,  0.995722430686905136L,  1L};
inline Matched matchedCorrection(const Matched& base, int type, FitReal f, FitReal a, FitReal q,
                                 FitReal fs) {
    // Fixed stable poles concentrate the two free numerator terms near Nyquist.
    // DC remains exact; weighted least squares fits relative squared magnitude.
    constexpr FitReal r = -.4L;
    Matched c;
    c.a1 = -2 * r;
    c.a2 = r * r;
    const auto D = phiPolynomial(1, c.a1, c.a2);
    const auto A = phiPolynomial(1, base.a1, base.a2);
    const auto B = phiPolynomial(base.b0, base.b1, base.b2);
    FitReal m11 = 0, m12 = 0, m22 = 0, t1 = 0, t2 = 0;
    for (std::size_t k = 0; k < kFitPhi.size(); ++k) {
        const auto phi = kFitPhi[k];
        const auto hz = fs * static_cast<FitReal>(k + 1) / 48;
        const auto residual =
            analogSquared(type, hz / f, a, q) * phiValue(A, phi) / phiValue(B, phi);
        const auto scale = residual * phiValue(D, phi);
        const auto u = phi / scale, v = 4 * phi * (1 - phi) / scale;
        const auto rhs = 1 - D[0] * (1 - phi) / scale;
        m11 += u * u;
        m12 += u * v;
        m22 += v * v;
        t1 += u * rhs;
        t2 += v * rhs;
    }
    const auto det = m11 * m22 - m12 * m12;
    factorNumerator(c, D[0], (t1 * m22 - t2 * m12) / det, (t2 * m11 - t1 * m12) / det);
    // Inversion of a boundary numerator would put poles exactly on the circle.
    if (std::abs(c.b2 / c.b0) >= .99L || std::abs(c.b1 / c.b0) >= .99L * (1 + c.b2 / c.b0))
        return {};
    return c;
}
inline Design matchedDesign(int type, double frequency, double gain, double qValue, int slope,
                            double rate) {
    Design d;
    const FitReal f = static_cast<FitReal>(frequency), fs = static_cast<FitReal>(rate),
                  q = static_cast<FitReal>(qValue), g = static_cast<FitReal>(gain);
    const auto w = 2 * std::numbers::pi_v<FitReal> * f / fs;
    auto add = [&](const Matched& c) { d.s[static_cast<std::size_t>(d.n++)] = c.section(); };
    if (type == Bell || type == LowShelf || type == HighShelf) {
        if (gain == 0) return d;
        const auto a = std::pow(FitReal{10}, std::abs(g) / 40);
        auto base = matchedBoost(type == Bell ? Bell : LowShelf, f, a, q, fs);
        auto correction = matchedCorrection(base, type == Bell ? Bell : LowShelf, f, a, q, fs);
        const bool invert = type == HighShelf ? gain > 0 : gain < 0;
        if (invert) {
            base.invert();
            correction.invert();
        }
        if (type == HighShelf) {
            const auto scale = std::pow(FitReal{10}, g / 20);
            base.b0 *= scale;
            base.b1 *= scale;
            base.b2 *= scale;
        }
        add(base);
        add(correction);
        return d;
    }
    auto first = [&](int filter, FitReal pole, FitReal level = 1) {
        Matched c;
        const auto r = std::exp(-pole);
        c.a1 = -r;
        const auto x = std::numbers::pi_v<FitReal> / pole;
        FitReal dc, ny;
        if (filter == LowPass || filter == HighPass) {
            dc = filter == LowPass ? 1 : 0;
            const auto phi = std::pow(std::sin(pole / 2), 2);
            const auto A0 = (1 - r) * (1 - r), A1 = (1 + r) * (1 + r);
            const auto B1 = (.5L * (A0 * (1 - phi) + A1 * phi) - dc * dc * A0 * (1 - phi)) / phi;
            ny = std::sqrt(std::max(FitReal{0}, B1) / A1);
        } else if (filter == LowShelf) {
            dc = level;
            ny = std::sqrt((level * level + x * x) / (1 + x * x));
        } else {
            dc = 1;
            ny = std::sqrt((1 + level * level * x * x) / (1 + x * x));
        }
        c.b0 = (dc * (1 - r) + ny * (1 + r)) / 2;
        c.b1 = (dc * (1 - r) - ny * (1 + r)) / 2;
        add(c);
    };
    if (type == Tilt) {
        if (gain == 0) return d;
        const auto low = std::pow(FitReal{10}, -g / 40), high = 1 / low;
        first(LowShelf, w / std::sqrt(low), low);
        first(HighShelf, w * std::sqrt(high), high);
        return d;
    }
    auto second = [&](int filter, FitReal qq) {
        auto c = matchedPoles(w, 1 / (2 * qq));
        const auto A = phiPolynomial(1, c.a1, c.a2);
        const auto phi = std::pow(std::sin(w / 2), 2);
        if (filter == HighPass) {
            c.b0 = qq * std::sqrt(std::max(FitReal{0}, phiValue(A, phi))) / (4 * phi);
            c.b1 = -2 * c.b0;
            c.b2 = c.b0;
        } else if (filter == Notch) {
            c.b0 = (1 + c.a1 + c.a2) / (4 * phi);
            c.b1 = -2 * c.b0 * std::cos(w);
            c.b2 = c.b0;
        } else {
            const auto x = fs / (2 * f);
            const auto den = (1 - x * x) * (1 - x * x) + x * x / (qq * qq);
            const auto B0 = filter == LowPass ? A[0] : 0;
            const auto B1 = (filter == LowPass ? 1 : x * x / (qq * qq)) / den * A[1];
            const auto centre = filter == LowPass ? qq * qq : 1;
            const auto B2 =
                (centre * phiValue(A, phi) - B0 * (1 - phi) - B1 * phi) / (4 * phi * (1 - phi));
            factorNumerator(c, B0, B1, B2);
        }
        add(c);
    };
    if (type == Notch || type == BandPass) {
        second(type, q);
        return d;
    }
    constexpr int orders[] = {1, 2, 3, 4, 6, 8, 12, 16};
    const int order = orders[slope];
    if (order % 2) first(type, w);
    for (int i = 0; i < order / 2; ++i)
        second(type,
               order == 2
                   ? q
                   : 1 / (2 * std::sin((2 * i + 1) * std::numbers::pi_v<FitReal> / (2 * order))));
    return d;
}
}  // namespace supple::eq
