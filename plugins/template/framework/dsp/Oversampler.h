// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Oversampling, 2x to 16x, for anything nonlinear: a saturator's harmonics
// above Nyquist fold back as inharmonic aliases unless they are made at a
// higher rate and filtered away before coming down.
//
//   prepare:  over_.prepare(setup.channels, setup.maxBlockSize, 4);
//             // latencySamples() returns over_.latency()
//   process:  for each channel c:
//                 double* hi = over_.up(c, b.channel(c), b.frames);
//                 for (int i = 0; i < b.frames * over_.factor(); ++i) hi[i] = shape(hi[i]);
//                 over_.down(c, b.channel(c), b.frames);
//
// Each doubling is a half-band filter, steep where the signal is (passband
// to 0.45 of the base rate, stopband 100 dB down from 0.55) and cheap above
// it. Two kinds:
//
//   - Phase::Linear: symmetric FIR half-bands. No phase change, so mixing
//     the result with the dry signal delayed by latency() is exact; the cost
//     is latency (about 70 samples at 2x, a few more beyond).
//   - Phase::Minimum: polyphase IIR all-pass half-bands (the elliptic design
//     of Laurent de Soras' HIIR). A few samples of delay at low frequencies,
//     more near Nyquist; latency() is the delay at low frequencies, rounded.
//
// latency() is in base-rate samples. prepare() allocates; nothing else does.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "dsp/Math.h"
#include "dsp/Fir.h"

namespace supple::dsp {

namespace oversampling {

// The passband edge, as a fraction of the base rate, that every stage keeps
// flat; and how far down the stopband is.
inline constexpr double kPassband = 0.45;
inline constexpr double kAttenuationDb = 100.0;

// A linear-phase half-band FIR, length 4m + 3, as two branches: the even
// taps, and a centre tap of 1/2 that is a pure delay. One per direction: its
// history is either an upsampler's or a downsampler's.
class FirStage {
  public:
    void design(double transition) {
        const int wanted = kaiserTaps(kAttenuationDb, transition);
        int length = 3;
        while (length < wanted) length += 4;
        const std::vector<double> h = lowpassKernel(length, 0.25, kAttenuationDb);
        // The odd taps are zero but for the centre, which is 1/2; the even
        // ones sum to the other 1/2, exactly, so DC passes unchanged.
        even_.clear();
        double sum = 0.0;
        for (int j = 0; j < length; j += 2) {
            even_.push_back(h[static_cast<size_t>(j)]);
            sum += h[static_cast<size_t>(j)];
        }
        for (double& v : even_) v *= 0.5 / sum;
        centre_ = (length - 1) / 2;  // odd
        size_ = even_.size();
        evenHistory_.assign(size_, 0.0);
        oddHistory_.assign(size_, 0.0);
        clear();
    }
    void clear() {
        std::fill(evenHistory_.begin(), evenHistory_.end(), 0.0);
        std::fill(oddHistory_.begin(), oddHistory_.end(), 0.0);
        write_ = 0;
    }
    // Delay of one pass, up or down, in samples of the higher rate.
    double delay() const { return centre_; }

    // n samples in, 2n out.
    void up(const double* in, int n, double* out) {
        const size_t d = static_cast<size_t>((centre_ - 1) / 2);
        for (int i = 0; i < n; ++i) {
            write_ = write_ == 0 ? size_ - 1 : write_ - 1;
            evenHistory_[write_] = in[i];
            out[2 * i] = 2.0 * dot(evenHistory_);
            out[2 * i + 1] = evenHistory_[(write_ + d) % size_];
        }
    }
    // 2n samples in, n out.
    void down(const double* in, int n, double* out) {
        const size_t d = static_cast<size_t>((centre_ + 1) / 2);
        for (int i = 0; i < n; ++i) {
            write_ = write_ == 0 ? size_ - 1 : write_ - 1;
            evenHistory_[write_] = in[2 * i];
            oddHistory_[write_] = in[2 * i + 1];
            out[i] = dot(evenHistory_) + 0.5 * oddHistory_[(write_ + d) % size_];
        }
    }

  private:
    // sum over k of even[k] * history[n - k], history newest at write_.
    double dot(const std::vector<double>& history) const {
        double y = 0.0;
        const size_t first = size_ - write_;
        for (size_t k = 0; k < first; ++k) y += even_[k] * history[write_ + k];
        for (size_t k = first; k < size_; ++k) y += even_[k] * history[k - first];
        return y;
    }

    std::vector<double> even_, evenHistory_, oddHistory_;
    size_t size_ = 1, write_ = 0;
    int centre_ = 1;
};

// A polyphase IIR half-band: two chains of first-order all-passes, one per
// phase. Coefficients from the elliptic design in HIIR (Laurent de Soras,
// WTFPL), computed here rather than tabulated.
class IirStage {
  public:
    static constexpr int kMaxCoefficients = 16;

    void design(double transition) {
        double k = 0.0, q = 0.0;
        transitionParameters(transition, k, q);
        const int order = orderFor(kAttenuationDb, q);
        count_ = std::min(kMaxCoefficients, (order - 1) / 2);
        for (int i = 0; i < count_; ++i) coefficient_[i] = coefficient(i, k, q, order);
        clear();
    }
    void clear() {
        for (auto& s : upState_) s = {};
        for (auto& s : downState_) s = {};
    }

    // Group delay of one pass at `fraction` of the higher rate, in samples
    // of that rate.
    double delay(double fraction) const {
        const double w = 2.0 * kPi * fraction, dw = 1e-5;
        const double a = std::arg(response(w)), b = std::arg(response(w + dw));
        double d = b - a;
        while (d > kPi) d -= 2.0 * kPi;
        while (d < -kPi) d += 2.0 * kPi;
        return -d / dw;
    }

    void up(const double* in, int n, double* out) {
        for (int i = 0; i < n; ++i) {
            double even = in[i], odd = in[i];
            for (int c = 0; c < count_; c += 2) even = upState_[c].tick(coefficient_[c], even);
            for (int c = 1; c < count_; c += 2) odd = upState_[c].tick(coefficient_[c], odd);
            out[2 * i] = even;
            out[2 * i + 1] = odd;
        }
    }
    void down(const double* in, int n, double* out) {
        for (int i = 0; i < n; ++i) {
            double a = in[2 * i + 1], b = in[2 * i];
            for (int c = 0; c < count_; c += 2) a = downState_[c].tick(coefficient_[c], a);
            for (int c = 1; c < count_; c += 2) b = downState_[c].tick(coefficient_[c], b);
            out[i] = 0.5 * (a + b);
        }
    }

  private:
    struct Section {
        double x1 = 0.0, y1 = 0.0;
        // (c + z^-1) / (1 + c z^-1)
        double tick(double c, double x) {
            const double y = c * (x - y1) + x1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    // H(z) = (A0(z^2) + z^-1 A1(z^2)) / 2 at the higher rate.
    std::complex<double> response(double w) const {
        const std::complex<double> z2 = std::polar(1.0, -2.0 * w), z1 = std::polar(1.0, -w);
        std::complex<double> a0 = 1.0, a1 = 1.0;
        for (int c = 0; c < count_; ++c) {
            const std::complex<double> s = (coefficient_[c] + z2) / (1.0 + coefficient_[c] * z2);
            (c % 2 == 0 ? a0 : a1) *= s;
        }
        return 0.5 * (a0 + z1 * a1);
    }

    static void transitionParameters(double transition, double& k, double& q) {
        k = std::tan((1.0 - transition * 2.0) * kPi / 4.0);
        k *= k;
        const double root = std::pow(1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - root) / (1.0 + root), e4 = e * e * e * e;
        q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    }
    static int orderFor(double attenuationDb, double q) {
        const double p = std::pow(10.0, -attenuationDb / 10.0), a = p / (1.0 - p);
        int order = static_cast<int>(std::ceil(std::log(a * a / 16.0) / std::log(q)));
        if (order % 2 == 0) ++order;
        return std::max(order, 3);
    }
    static double coefficient(int index, double k, double q, int order) {
        const int c = index + 1;
        double num = 0.0, den = 0.0, term = 0.0;
        int sign = 1;
        for (int i = 0; i < 64; ++i, sign = -sign) {
            term = std::pow(q, i * (i + 1)) * std::sin((i * 2 + 1) * c * kPi / order) * sign;
            num += term;
            if (std::fabs(term) < 1e-100) break;
        }
        sign = -1;
        for (int i = 1; i < 64; ++i, sign = -sign) {
            term = std::pow(q, i * i) * std::cos(i * 2 * c * kPi / order) * sign;
            den += term;
            if (std::fabs(term) < 1e-100) break;
        }
        const double ww = num * std::pow(q, 0.25) / (den + 0.5), wwsq = ww * ww;
        const double x = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        return (1.0 - x) / (1.0 + x);
    }

    double coefficient_[kMaxCoefficients] = {};
    int count_ = 0;
    Section upState_[kMaxCoefficients], downState_[kMaxCoefficients];
};

}  // namespace oversampling

class Oversampler {
  public:
    enum class Phase { Linear, Minimum };
    static constexpr int kMaxStages = 4;  // 16x

    // factor: 1, 2, 4, 8 or 16 (others round down to one of those).
    // Allocates.
    void prepare(int channels, int maxBlock, int factor, Phase phase = Phase::Linear) {
        stages_ = 0;
        while (stages_ < kMaxStages && (2 << stages_) <= factor) ++stages_;
        factor_ = 1 << stages_;
        phase_ = phase;
        channels_ = std::max(0, channels);
        maxBlock_ = std::max(1, maxBlock);
        const size_t hi = static_cast<size_t>(maxBlock_ * factor_);
        lanes_.assign(static_cast<size_t>(channels_), Lane{});
        for (Lane& lane : lanes_) {
            lane.work.assign(hi, 0.0);
            lane.scratch.assign(hi, 0.0);
            for (int s = 0; s < stages_; ++s) {
                // Stage s doubles from 2^s to 2^(s+1) times the base rate:
                // the signal (to kPassband of the base) is a smaller fraction
                // of each higher rate, so later transitions are wider.
                const double pass = oversampling::kPassband / (1 << (s + 1));
                const double transition = 0.5 - 2.0 * pass;
                lane.firUp[s].design(transition);
                lane.firDown[s].design(transition);
                lane.iir[s].design(transition);
            }
        }
        computeLatency();
        for (Lane& lane : lanes_) lane.pad.assign(static_cast<size_t>(pad_) + 1, 0.0);
        reset();
    }

    void reset() {
        for (Lane& lane : lanes_) {
            for (int s = 0; s < stages_; ++s) {
                lane.firUp[s].clear();
                lane.firDown[s].clear();
                lane.iir[s].clear();
            }
            std::fill(lane.pad.begin(), lane.pad.end(), 0.0);
            lane.padWrite = 0;
        }
    }

    int factor() const { return factor_; }
    // Base-rate samples from input to output; report it to the host.
    int latency() const { return latency_; }

    // frames base-rate samples in; frames * factor() out, in a buffer the
    // caller may process in place before down(). frames <= maxBlock.
    double* up(int channel, const double* in, int frames) {
        Lane& lane = lanes_[static_cast<size_t>(channel)];
        frames = std::min(frames, maxBlock_);
        if (stages_ == 0) {
            std::copy_n(in, frames, lane.work.data());
            return lane.work.data();
        }
        // Ping-pong so the last stage lands in work.
        double* buffers[2] = {lane.work.data(), lane.scratch.data()};
        const double* source = in;
        int n = frames;
        for (int s = 0; s < stages_; ++s) {
            double* target = buffers[(stages_ - 1 - s) % 2];
            if (phase_ == Phase::Linear) lane.firUp[s].up(source, n, target);
            else lane.iir[s].up(source, n, target);
            source = target;
            n *= 2;
        }
        return lane.work.data();
    }

    // The buffer up() returned, back down to frames base-rate samples.
    void down(int channel, double* out, int frames) {
        Lane& lane = lanes_[static_cast<size_t>(channel)];
        frames = std::min(frames, maxBlock_);
        if (stages_ == 0) {
            std::copy_n(lane.work.data(), frames, out);
            return;
        }
        int n = frames * factor_;
        if (pad_ > 0) {
            // Whole base-rate samples of latency, by delaying at the top rate.
            double* x = lane.work.data();
            const size_t size = lane.pad.size();
            for (int i = 0; i < n; ++i) {
                lane.pad[lane.padWrite] = x[i];
                x[i] = lane.pad[(lane.padWrite + 1) % size];
                lane.padWrite = (lane.padWrite + 1) % size;
            }
        }
        const double* source = lane.work.data();
        for (int s = stages_ - 1; s >= 0; --s) {
            n /= 2;
            double* target = s == 0 ? out : source == lane.work.data() ? lane.scratch.data() : lane.work.data();
            if (phase_ == Phase::Linear) lane.firDown[s].down(source, n, target);
            else lane.iir[s].down(source, n, target);
            source = target;
        }
    }

  private:
    struct Lane {
        std::vector<double> work, scratch, pad;
        size_t padWrite = 0;
        oversampling::FirStage firUp[kMaxStages], firDown[kMaxStages];
        oversampling::IirStage iir[kMaxStages];
    };

    void computeLatency() {
        pad_ = 0;
        latency_ = 0;
        if (lanes_.empty() || stages_ == 0) return;
        const Lane& lane = lanes_[0];
        if (phase_ == Phase::Linear) {
            // In samples of the top rate: each stage's up and down delays,
            // scaled from its own rate.
            int top = 0;
            for (int s = 0; s < stages_; ++s)
                top += 2 * static_cast<int>(lane.firUp[s].delay()) * (factor_ >> (s + 1));
            pad_ = (factor_ - top % factor_) % factor_;
            latency_ = (top + pad_) / factor_;
        } else {
            double base = 0.0;
            for (int s = 0; s < stages_; ++s) {
                const double rate = static_cast<double>(2 << s);  // this stage's higher rate
                // Down takes the odd phase of each pair: one sample sooner.
                base += (2.0 * lane.iir[s].delay(0.01 / rate) - 1.0) / rate;
            }
            latency_ = static_cast<int>(std::lround(base));
        }
    }

    std::vector<Lane> lanes_;
    Phase phase_ = Phase::Linear;
    int stages_ = 0, factor_ = 1, channels_ = 0, maxBlock_ = 1, latency_ = 0, pad_ = 0;
};

}  // namespace supple::dsp
