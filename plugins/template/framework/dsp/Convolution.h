// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Convolution with a long impulse response -- a reverb's room, a speaker
// cabinet, a microphone -- by uniformly partitioned overlap-save in the
// frequency domain, with no latency: the first partition is convolved
// directly, sample by sample, and the FFT handles the rest, whose own
// latency is exactly that first partition's length.
//
// Building one partitions and transforms the whole response, which
// allocates and takes a while for a long one: do it in prepare(), or off the
// audio thread (a processor's loadFile(), dsp/Worker.h) and hand the
// finished object over. process() allocates nothing.
//
//   auto room = std::make_unique<dsp::Convolution>();
//   room->prepare(ir.data().data(), ir.channels(), ir.frames(), setup.channels);
//   ...
//   room->process(c, b.channel(c), b.frames);    // in place, per channel
//
// Each channel of the audio uses the response's channel of the same number,
// or its last one: a mono response serves every channel. CPU grows with the
// response's length divided by the partition (256 by default); the direct
// part costs `partition` multiply-adds a sample.
#pragma once

#include <algorithm>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/Fft.h"

namespace supple::dsp {

class Convolution {
  public:
    Convolution() = default;
    // Each channel points into the kernels: moving keeps them, copying would not.
    Convolution(const Convolution&) = delete;
    Convolution& operator=(const Convolution&) = delete;
    Convolution(Convolution&&) = default;
    Convolution& operator=(Convolution&&) = default;

    // T: float or double. `zeroLatency` false skips the direct part and
    // delays everything by `partition` samples (latency()), for less CPU.
    template <class T>
    void prepare(const T* const* ir, int irChannels, size_t length, int channels, int partition = 256,
                 bool zeroLatency = true) {
        size_t p = 16;
        while (p * 2 <= static_cast<size_t>(std::max(16, partition))) p *= 2;
        p_ = p;
        zeroLatency_ = zeroLatency;
        length_ = length;
        fft_.resize(2 * p_);
        frame_.assign(2 * p_, {});
        sum_.assign(p_ + 1, {});

        irChannels = std::max(1, irChannels);
        kernels_.assign(static_cast<size_t>(irChannels), Kernel{});
        const size_t start = zeroLatency_ ? std::min(p_, length) : 0;  // the FFT's first tap
        partitions_ = (length - start + p_ - 1) / p_;
        for (int k = 0; k < irChannels; ++k) {
            Kernel& kernel = kernels_[static_cast<size_t>(k)];
            const T* h = ir ? ir[k] : nullptr;
            kernel.head.assign(p_, 0.0);
            if (zeroLatency_ && h)
                for (size_t n = 0; n < std::min(p_, length); ++n) kernel.head[n] = static_cast<double>(h[n]);
            kernel.spectra.assign(partitions_ * (p_ + 1), {});
            for (size_t part = 0; part < partitions_; ++part) {
                std::fill(frame_.begin(), frame_.end(), std::complex<double>{});
                for (size_t n = 0; n < p_; ++n) {
                    const size_t tap = start + part * p_ + n;
                    if (h && tap < length) frame_[n] = static_cast<double>(h[tap]);
                }
                fft_.forward(frame_.data());
                std::copy_n(frame_.begin(), p_ + 1, kernel.spectra.begin() + static_cast<std::ptrdiff_t>(part * (p_ + 1)));
            }
        }

        lanes_.assign(static_cast<size_t>(std::max(0, channels)), Lane{});
        for (size_t c = 0; c < lanes_.size(); ++c) {
            Lane& lane = lanes_[c];
            lane.kernel = &kernels_[std::min(c, kernels_.size() - 1)];
            lane.history.assign(2 * p_, 0.0);
            lane.input.assign(2 * p_, 0.0);
            lane.output.assign(p_, 0.0);
            lane.delayLine.assign(std::max<size_t>(partitions_, 1) * (p_ + 1), {});
        }
        reset();
    }

    void reset() {
        for (Lane& lane : lanes_) {
            std::fill(lane.history.begin(), lane.history.end(), 0.0);
            std::fill(lane.input.begin(), lane.input.end(), 0.0);
            std::fill(lane.output.begin(), lane.output.end(), 0.0);
            std::fill(lane.delayLine.begin(), lane.delayLine.end(), std::complex<double>{});
            lane.write = 0;
            lane.position = 0;
            lane.newest = 0;
        }
    }

    int latency() const { return zeroLatency_ ? 0 : static_cast<int>(p_); }
    size_t length() const { return length_; }
    int channels() const { return static_cast<int>(lanes_.size()); }

    // In place: x holds the input on entry and the convolved signal on return.
    void process(int channel, double* x, int frames) {
        Lane& lane = lanes_[static_cast<size_t>(channel)];
        const std::vector<double>& head = lane.kernel->head;
        for (int i = 0; i < frames; ++i) {
            const double in = x[i];
            double y = lane.output[lane.position];
            if (zeroLatency_) {
                // history[write + k] is the input k samples ago.
                lane.write = lane.write == 0 ? p_ - 1 : lane.write - 1;
                lane.history[lane.write] = lane.history[lane.write + p_] = in;
                const double* past = lane.history.data() + lane.write;
                double direct = 0.0;
                for (size_t k = 0; k < p_; ++k) direct += head[k] * past[k];
                y += direct;
            }
            lane.input[p_ + lane.position] = in;
            x[i] = y;
            if (++lane.position == p_) {
                lane.position = 0;
                block(lane);
            }
        }
    }

  private:
    struct Kernel {
        std::vector<double> head;                   // the taps convolved directly
        std::vector<std::complex<double>> spectra;  // each partition's bins 0..p, in order
    };
    struct Lane {
        const Kernel* kernel = nullptr;
        std::vector<double> history;                  // the last p inputs, twice over
        std::vector<double> input;                    // the previous block, then this one
        std::vector<double> output;                   // what the last block made, to play next
        std::vector<std::complex<double>> delayLine;  // each past block's spectrum, newest at `newest`
        size_t write = 0, position = 0, newest = 0;
    };

    // A whole block of input has arrived: its spectrum joins the delay line,
    // and every partition's product, summed, is the next block's output.
    void block(Lane& lane) {
        if (partitions_ == 0) return;
        for (size_t n = 0; n < 2 * p_; ++n) frame_[n] = lane.input[n];
        std::copy(lane.input.begin() + static_cast<std::ptrdiff_t>(p_), lane.input.end(), lane.input.begin());
        fft_.forward(frame_.data());
        lane.newest = (lane.newest + 1) % partitions_;
        std::copy_n(frame_.begin(), p_ + 1, lane.delayLine.begin() + static_cast<std::ptrdiff_t>(lane.newest * (p_ + 1)));

        std::fill(sum_.begin(), sum_.end(), std::complex<double>{});
        const std::complex<double>* h = lane.kernel->spectra.data();
        for (size_t part = 0; part < partitions_; ++part) {
            const size_t slot = (lane.newest + partitions_ - part) % partitions_;
            const std::complex<double>* xs = lane.delayLine.data() + slot * (p_ + 1);
            const std::complex<double>* hs = h + part * (p_ + 1);
            for (size_t b = 0; b <= p_; ++b) sum_[b] += hs[b] * xs[b];
        }
        for (size_t b = 0; b <= p_; ++b) frame_[b] = sum_[b];
        for (size_t b = 1; b < p_; ++b) frame_[2 * p_ - b] = std::conj(sum_[b]);
        fft_.inverse(frame_.data());
        // Overlap-save: the second half is the linear convolution.
        for (size_t n = 0; n < p_; ++n) lane.output[n] = frame_[p_ + n].real();
    }

    Fft fft_{32};
    std::vector<std::complex<double>> frame_, sum_;
    std::vector<Kernel> kernels_;
    std::vector<Lane> lanes_;
    size_t p_ = 16, partitions_ = 0, length_ = 0;
    bool zeroLatency_ = true;
};

}  // namespace supple::dsp
