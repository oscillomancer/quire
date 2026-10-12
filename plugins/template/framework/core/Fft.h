// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A small radix-2 FFT: complex, in place, any power-of-two size, tables built
// once. resize() allocates; forward() and inverse() do not, so it serves the
// audio thread too (dsp/Stft.h, dsp/Convolution.h). Plain and deterministic
// rather than fast: repeated transforms with the same build and floating-point
// environment are reproducible. Different compilers and math libraries may
// differ in their last bits.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

namespace supple::dsp {

class Fft {
  public:
    explicit Fft(size_t size = 2048) { resize(size); }

    // Rounds down to a power of two, at least 2.
    void resize(size_t size) {
        size_t n = 2;
        while (n * 2 <= size) n *= 2;
        n_ = n;
        twiddle_.resize(n_ / 2);
        for (size_t k = 0; k < n_ / 2; ++k)
            twiddle_[k] = std::polar(1.0, -2.0 * 3.14159265358979323846 * static_cast<double>(k) / static_cast<double>(n_));
        reversed_.resize(n_);
        size_t bits = 0;
        while ((size_t{1} << bits) < n_) ++bits;
        for (size_t i = 0; i < n_; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b)
                if (i >> b & 1) r |= size_t{1} << (bits - 1 - b);
            reversed_[i] = r;
        }
    }
    size_t size() const { return n_; }

    // x[k] = sum over n of x[n] e^(-2 pi i k n / N), in place; x holds size().
    void forward(std::complex<double>* x) const {
        for (size_t i = 0; i < n_; ++i)
            if (i < reversed_[i]) std::swap(x[i], x[reversed_[i]]);
        for (size_t half = 1; half < n_; half *= 2) {
            const size_t stride = n_ / (half * 2);
            for (size_t start = 0; start < n_; start += half * 2) {
                for (size_t k = 0; k < half; ++k) {
                    const std::complex<double> t = twiddle_[k * stride] * x[start + k + half];
                    x[start + k + half] = x[start + k] - t;
                    x[start + k] += t;
                }
            }
        }
    }

    // The inverse, scaled by 1/N: inverse(forward(x)) is x.
    void inverse(std::complex<double>* x) const {
        for (size_t i = 0; i < n_; ++i) x[i] = std::conj(x[i]);
        forward(x);
        const double scale = 1.0 / static_cast<double>(n_);
        for (size_t i = 0; i < n_; ++i) x[i] = std::conj(x[i]) * scale;
    }

  private:
    size_t n_ = 0;
    std::vector<std::complex<double>> twiddle_;
    std::vector<size_t> reversed_;
};

}  // namespace supple::dsp
