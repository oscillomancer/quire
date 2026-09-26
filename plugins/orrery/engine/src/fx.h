// Global post-FX (synth.md §4.10): chorus, phaser, delay, reverb, EQ.
// Stereo, base rate. set() takes the block's effective physical values in
// field order (ChorusF, PhaserF, ...) once per control tick; process() runs
// ≤ one tick of samples. prepare() allocates; everything else is RT-safe.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "orrery/params.h"
#include "orrery/patch.h"
#include "supple/base/rt.h"

namespace supple::orrery {

class DelayLine {
  public:
    void allocate(std::size_t samples) { buf_.assign(samples + 4, 0.0f); }
    void reset() SUPPLE_RT {
        for (float& f : buf_) f = 0.0f;
        pos_ = 0;
    }
    void write(float x) SUPPLE_RT {
        buf_[pos_] = x;
        pos_ = pos_ + 1 == buf_.size() ? 0 : pos_ + 1;
    }
    // Delay ≥ 1 sample, fractional (linear interpolation).
    double read(double delay) const SUPPLE_RT;
    std::size_t capacity() const { return buf_.size() - 4; }

  private:
    std::vector<float> buf_;
    std::size_t pos_ = 0;
};

class Biquad {
  public:
    void reset() SUPPLE_RT { z1_ = z2_ = 0.0; }
    void set(double b0, double b1, double b2, double a1, double a2) SUPPLE_RT {
        b0_ = b0, b1_ = b1, b2_ = b2, a1_ = a1, a2_ = a2;
    }
    double process(double x) SUPPLE_RT {
        const double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }
    // Audio EQ Cookbook (R. Bristow-Johnson) forms.
    void lowShelf(double fs, double hz, double db) SUPPLE_RT;
    void highShelf(double fs, double hz, double db) SUPPLE_RT;
    void peak(double fs, double hz, double db, double q) SUPPLE_RT;

  private:
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0, z1_ = 0.0, z2_ = 0.0;
};

class FxRack {
  public:
    void prepare(double sampleRate);  // main thread; allocates delay lines
    void reset() SUPPLE_RT;
    // values: the block's fields in order; tempo in BPM (synced delay).
    void set(FxKind kind, std::span<const double> values, double tempo) SUPPLE_RT;
    void process(FxKind kind, float* left, float* right, std::uint32_t n) SUPPLE_RT;

  private:
    double fs_ = 48000.0;

    struct Chorus {
        std::array<DelayLine, 2> line;
        double phase = 0.0, rate = 0.5, depth = 0.3, mix = 0.5;
        int voices = 2;
    } chorus_;

    struct Phaser {
        std::array<std::array<double, 12>, 2> ap{};  // all-pass states per channel
        std::array<double, 2> fb{};
        double phase = 0.0, rate = 0.3, depth = 0.5, feedback = 0.3, mix = 0.5;
        int stages = 6;
    } phaser_;

    struct Delay {
        std::array<DelayLine, 2> line;
        std::array<double, 2> lp{};
        double samples = 0.0, target = 0.0, feedback = 0.35, damping = 0.3, mix = 0.25;
        bool pingPong = false;
    } delay_;

    struct Reverb {
        static constexpr int kLines = 8;
        std::array<DelayLine, kLines> line;
        std::array<double, kLines> lp{}, gain{}, length{};
        std::array<DelayLine, 2> pre;
        double predelay = 0.0, damping = 0.4, width = 1.0, mix = 0.2;
    } reverb_;

    struct Eq {
        std::array<Biquad, 2> low, mid, high;
    } eq_;
};

}  // namespace supple::orrery
