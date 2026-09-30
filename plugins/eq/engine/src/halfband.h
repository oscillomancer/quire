// Polyphase IIR half-band decimators (voice bus → base rate).
//
// H(z) = ½·[A0(z²) + z⁻¹·A1(z²)], each Ai a cascade of first-order all-pass
// sections (aᵢ + z⁻²)/(1 + aᵢ·z⁻²): an odd-order elliptic half-band low-pass
// (Valenzuela & Constantinides, 1983). Coefficients come from the classical
// elliptic-function formulas (nome series, Abramowitz & Stegun 17.3.21; the
// half-band pole positions via theta-function series). Minimum-phase-like,
// no reported latency. Voices are summed at the oversampled rate first, so
// one decimator serves all voices.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "supple/base/rt.h"

namespace supple::eq {

// `count` all-pass coefficients (filter order 2·count + 1) for a transition
// band of `transition` (fraction of the input rate, centred on a quarter of
// it). Sorted ascending; even indices belong to branch 0, odd to branch 1.
std::vector<double> designHalfband(int count, double transition);

// Magnitude response in dB at `f` (fraction of the input rate), for tests.
double halfbandResponseDb(const std::vector<double>& coefs, double f);

class Halfband {
  public:
    static constexpr int kMaxCoefs = 12;
    void setCoefs(const std::vector<double>& coefs);  // main thread
    void reset() SUPPLE_RT;
    // Two input samples in, one out.
    double process(double even, double odd) SUPPLE_RT;
    void up(double input, double& even, double& odd) SUPPLE_RT;

  private:
    struct Section {
        double a = 0.0, x1 = 0.0, y1 = 0.0;
    };
    std::array<Section, kMaxCoefs> s_{};
    int n0_ = 0, n1_ = 0;  // sections per branch (branch 0 first, then branch 1)
    double oddPrev_ = 0.0;
};

}  // namespace supple::eq
