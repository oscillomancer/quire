// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "supple/base/rt.h"

namespace supple::dsp {
// One channel, factors 1/2/4/8. The first stage uses 1025 taps; subsequent stages use
// 257 taps. All are symmetric half-band sincs, Kaiser beta 12, cutoff pi/2. Even taps except the
// centre are exactly zero. The 0.45..0.55 transition (relative to the lower rate) gives >100 dB
// rejection and <0.01 dB ripple through 0.45 of the base rate. The first stage has a narrower
// transition to reject a 25 kHz harmonic before decimation to 48 kHz. Up/down delay is
// 0/512/576/608 base samples. No fractional phase or host-block-dependent state. Storage is fixed
// for the largest factor, so prepare needs no per-block scratch.
class Oversampler {
  public:
    void prepare(int factor, uint32_t maxFrames = 0);
    void reset() SUPPLE_RT;
    int factor() const { return factor_; }
    uint32_t latency() const { return latency_; }
    void upsample(double input, std::span<double> output) SUPPLE_RT;
    double downsample(std::span<const double> input) SUPPLE_RT;

  private:
    struct Stage {
        std::array<double, 1025> h{};
        std::array<double, 2048> down{};
        std::array<double, 1024> up{};
        size_t center = 512;
        size_t upHead = 0, downHead = 0;
        void interpolate(double x, double* out) SUPPLE_RT;
        double decimate(const double* in) SUPPLE_RT;
    };
    std::array<Stage, 3> stages_{};
    int factor_ = 1, count_ = 0;
    uint32_t latency_ = 0;
};
}  // namespace supple::dsp
