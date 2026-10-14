// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <array>
#include <complex>
#include <vector>
#include "eq/processor.h"
#include "supple/base/handoff.h"
namespace supple::eq {
using Complex = std::complex<double>;
void fft(std::span<Complex>, bool inverse) SUPPLE_RT;
struct Kernel {
    std::vector<Complex> spectra;
    std::vector<double> impulses;
};
double linearMagnitudeDb(const Values&, double hz, double fs);
class Linear {
  public:
    static constexpr std::size_t kBlock = 128, kFft = 256;
    void prepare(double fs);
    void publish(const Values&, double fs);
    void collect() { handoff_.collect(); }
    void reset() SUPPLE_RT;
    void sample(double& l, double& r) SUPPLE_RT;
    int latency() const { return latency_; }

  private:
    void compute(std::size_t position, std::array<std::array<double, kFft>, 2>& out) SUPPLE_RT;
    RtHandoff<Kernel> handoff_;
    std::size_t parts_ = 0, head_ = 0, fill_ = 0;
    int latency_ = 2048, fade_ = 0, fadeLength_ = 960;
    std::vector<Complex> history_, current_, target_;
    std::array<std::array<Complex, kFft>, 2> input_{};
    std::array<std::array<double, kFft>, 2> output_{}, previous_{};
    std::array<Complex, kFft> scratch_{};
};
}  // namespace supple::eq
