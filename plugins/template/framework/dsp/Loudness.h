// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Loudness as broadcasters measure it (ITU-R BS.1770-4, EBU R 128):
// K-weighted, gated, in LUFS -- momentary (400 ms), short-term (3 s) and
// integrated over everything since reset(). And a stereo correlation meter.
//
//   prepare:  loudness_.prepare(setup.sampleRate, setup.channels);
//   process:  const double* ch[2] = {b.channel(0), b.channel(1)};
//             loudness_.process(ch, b.frames);        // or tick(frame), a frame at a time
//             setOutput(kLoudness, loudness_.shortTerm());
//
// Channel weights default to 1; for 5.1 set the surrounds to 1.41 and the
// LFE to 0 (BS.1770's G). Up to 16 channels. prepare() allocates; the rest
// does not.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/Filters.h"

namespace supple::dsp {

// BS.1770's K-weighting: a high shelf (+4 dB above about 1.5 kHz, the head's
// effect) then a high-pass (RLB, below about 60 Hz), designed for any rate
// from the specification's analog prototype (as libebur128 does). At 48 kHz
// it is the specification's table, to its last digit.
struct KWeighting {
    Biquad shelf, highpass;

    static KWeighting design(double sampleRate) {
        KWeighting k;
        {
            const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
            const double K = std::tan(kPi * f0 / sampleRate);
            const double vh = std::pow(10.0, gain / 20.0), vb = std::pow(vh, 0.4996667741545416);
            const double a0 = 1.0 + K / q + K * K;
            k.shelf.b0 = (vh + vb * K / q + K * K) / a0;
            k.shelf.b1 = 2.0 * (K * K - vh) / a0;
            k.shelf.b2 = (vh - vb * K / q + K * K) / a0;
            k.shelf.a1 = 2.0 * (K * K - 1.0) / a0;
            k.shelf.a2 = (1.0 - K / q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double K = std::tan(kPi * f0 / sampleRate);
            const double a0 = 1.0 + K / q + K * K;
            k.highpass.b0 = 1.0;
            k.highpass.b1 = -2.0;
            k.highpass.b2 = 1.0;
            k.highpass.a1 = 2.0 * (K * K - 1.0) / a0;
            k.highpass.a2 = (1.0 - K / q + K * K) / a0;
        }
        return k;
    }
};

class Loudness {
  public:
    static constexpr double kFloor = -70.0;  // LUFS: what silence reads, and the absolute gate

    // Allocates.
    void prepare(double sampleRate, int channels) {
        channels_ = std::clamp(channels, 1, kMaxChannels);
        weighting_ = KWeighting::design(sampleRate);
        state_.assign(static_cast<size_t>(channels_), Lane{});
        weight_.assign(static_cast<size_t>(channels_), 1.0);
        blockLength_ = std::max(1, static_cast<int>(std::lround(0.1 * sampleRate)));
        histogramCount_.assign(kBins, 0);
        histogramEnergy_.assign(kBins, 0.0);
        reset();
    }
    void setWeight(int channel, double weight) {
        if (channel >= 0 && channel < channels_) weight_[static_cast<size_t>(channel)] = weight;
    }
    // Clears everything, the integrated measurement included.
    void reset() {
        for (Lane& l : state_) l = Lane{};
        for (double& b : blocks_) b = 0.0;
        newest_ = 0;
        blocksSeen_ = 0;
        accumulated_ = 0.0;
        inBlock_ = 0;
        std::fill(histogramCount_.begin(), histogramCount_.end(), 0);
        std::fill(histogramEnergy_.begin(), histogramEnergy_.end(), 0.0);
    }

    // One frame: a sample for each channel.
    void tick(const double* frame) {
        double sum = 0.0;
        for (int c = 0; c < channels_; ++c) {
            Lane& l = state_[static_cast<size_t>(c)];
            const double y = weighting_.highpass.tick(l.highpass, weighting_.shelf.tick(l.shelf, frame[c]));
            sum += weight_[static_cast<size_t>(c)] * y * y;
        }
        accumulated_ += sum;
        if (++inBlock_ == blockLength_) closeBlock();
    }
    // Planar channels, as a Block holds them.
    void process(const double* const* channels, int frames) {
        double frame[kMaxChannels] = {};
        const int n = std::min(channels_, kMaxChannels);
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < n; ++c) frame[c] = channels[c][i];
            tick(frame);
        }
    }

    // LUFS, kFloor when there is nothing to measure yet.
    // Histogram gates have 0.01 LU resolution through +100 LUFS. Above
    // that, one overflow group retains exact summed energy and count. It
    // always passes the relative gate: its mean is at least the ungated
    // mean. If the gate itself exceeds +100, individual overflow blocks
    // cannot be separated. The reported value then under-reads exact gating
    // by at most max(overflow block LUFS) - LUFS(overflow mean energy).
    // A constant overflow level remains exact; no loud group becomes silence.
    double momentary() const { return lufs(mean(4)); }
    double shortTerm() const { return lufs(mean(30)); }
    double integrated() const {
        // The absolute gate, then a relative one 10 LU under what passed it.
        double energy = 0.0;
        long long count = 0;
        for (int b = 0; b < kBins; ++b) {
            energy += histogramEnergy_[static_cast<size_t>(b)];
            count += histogramCount_[static_cast<size_t>(b)];
        }
        if (count == 0) return kFloor;
        const double gate = lufs(energy / static_cast<double>(count)) - 10.0;
        energy = 0.0;
        count = 0;
        for (int b = std::clamp(bin(gate), 0, kBins - 1); b < kBins; ++b) {
            energy += histogramEnergy_[static_cast<size_t>(b)];
            count += histogramCount_[static_cast<size_t>(b)];
        }
        return count == 0 ? kFloor : lufs(energy / static_cast<double>(count));
    }

  private:
    static constexpr int kMaxChannels = 16;
    static constexpr int kHistory = 30;  // 100 ms blocks: 3 seconds
    // Normal bins cover [-70, +100) in 0.01 LU; the last is overflow.
    static constexpr int kBins = 17001;
    struct Lane {
        Biquad::State shelf, highpass;
    };

    static double lufs(double meanSquare) {
        return meanSquare > 0.0 ? std::max(kFloor, -0.691 + 10.0 * std::log10(meanSquare)) : kFloor;
    }
    static int bin(double loudness) { return static_cast<int>(std::floor((loudness - kFloor) * 100.0)); }

    // The mean square over the newest `count` 100 ms blocks.
    double mean(int count) const {
        if (blocksSeen_ < static_cast<uint64_t>(count)) count = static_cast<int>(blocksSeen_);
        if (count == 0) return 0.0;
        double sum = 0.0;
        for (int k = 0; k < count; ++k) sum += blocks_[(newest_ + kHistory - k) % kHistory];
        return sum / count;
    }

    void closeBlock() {
        newest_ = (newest_ + 1) % kHistory;
        blocks_[newest_] = accumulated_ / blockLength_;
        accumulated_ = 0.0;
        inBlock_ = 0;
        ++blocksSeen_;
        // Gating blocks are 400 ms, overlapping by 75%: one each 100 ms once
        // four have passed.
        if (blocksSeen_ >= 4) {
            const double energy = mean(4);
            const double loudness = lufs(energy);
            if (energy > 0.0 && -0.691 + 10.0 * std::log10(energy) > kFloor) {
                const int b = std::clamp(bin(loudness), 0, kBins - 1);
                ++histogramCount_[static_cast<size_t>(b)];
                histogramEnergy_[static_cast<size_t>(b)] += energy;
            }
        }
    }

    KWeighting weighting_;
    std::vector<Lane> state_;
    std::vector<double> weight_;
    double blocks_[kHistory] = {};
    int newest_ = 0;
    uint64_t blocksSeen_ = 0;
    double accumulated_ = 0.0;
    int inBlock_ = 0, blockLength_ = 4800, channels_ = 2;
    std::vector<long long> histogramCount_;
    std::vector<double> histogramEnergy_;
};

// How alike the left and right channels are, over about `ms`: +1 the same
// (mono), 0 unrelated, -1 one the inverse of the other (cancels in mono).
class Correlation {
  public:
    void prepare(double sampleRate, double ms = 300.0) {
        const double samples = std::max(1.0, ms * 0.001 * sampleRate);
        c_ = std::exp(-1.0 / samples);
        reset();
    }
    void reset() { lr_ = ll_ = rr_ = 0.0; }
    void tick(double left, double right) {
        lr_ = left * right + c_ * (lr_ - left * right);
        ll_ = left * left + c_ * (ll_ - left * left);
        rr_ = right * right + c_ * (rr_ - right * right);
    }
    // 0 while silent.
    double value() const {
        const double d = std::sqrt(ll_ * rr_);
        return d > 1e-12 ? std::clamp(lr_ / d, -1.0, 1.0) : 0.0;
    }

  private:
    double c_ = 0.0, lr_ = 0.0, ll_ = 0.0, rr_ = 0.0;
};

}  // namespace supple::dsp
