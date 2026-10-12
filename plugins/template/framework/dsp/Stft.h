// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The short-time Fourier transform, with overlap-add back to audio: the
// frame for spectral effects (filters drawn in frequency, denoisers, freezes,
// vocoders) and analysers.
//
//   prepare:  stft_.prepare(setup.channels, 2048);   // latencySamples(): stft_.latency()
//   process:  stft_.process(c, b.channel(c), b.frames, [&](std::complex<double>* bins, int count) {
//                 for (int k = 0; k < count; ++k) bins[k] *= gainAt(k);   // bin k is k * rate / size Hz
//             });
//
// Frames of `size` samples every size / overlap, Hann-windowed on the way in
// and again on the way out, and scaled so that a callback that changes
// nothing gives back the input exactly, latency() samples later. The
// callback sees bins 0 to size / 2 of a real signal's spectrum; the rest is
// their mirror image, which the inverse rebuilds. prepare() allocates;
// process() does not.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "dsp/Math.h"
#include "core/Fft.h"

namespace supple::dsp {

class Stft {
  public:
    // size: a power of two (rounded down); overlap: 4 or 8 frames over each
    // sample.
    void prepare(int channels, int size = 2048, int overlap = 4) {
        fft_.resize(static_cast<size_t>(std::max(16, size)));
        size_ = static_cast<int>(fft_.size());
        overlap_ = overlap >= 8 ? 8 : 4;
        hop_ = size_ / overlap_;
        window_.resize(static_cast<size_t>(size_));
        for (int n = 0; n < size_; ++n) window_[static_cast<size_t>(n)] = 0.5 - 0.5 * std::cos(2.0 * kPi * n / size_);
        // A periodic Hann, squared, overlapped this way sums to 3 * overlap / 8.
        scale_ = 8.0 / (3.0 * overlap_);
        frame_.assign(static_cast<size_t>(size_), {});
        lanes_.assign(static_cast<size_t>(std::max(0, channels)), Lane{});
        for (Lane& lane : lanes_) {
            lane.input.assign(static_cast<size_t>(size_), 0.0);
            lane.output.assign(static_cast<size_t>(size_), 0.0);
        }
        reset();
    }
    void reset() {
        for (Lane& lane : lanes_) {
            std::fill(lane.input.begin(), lane.input.end(), 0.0);
            std::fill(lane.output.begin(), lane.output.end(), 0.0);
            lane.position = 0;
            lane.untilFrame = hop_;
        }
    }

    int size() const { return size_; }
    int hop() const { return hop_; }
    int bins() const { return size_ / 2 + 1; }
    int latency() const { return size_ - 1; }
    // The centre frequency of bin k.
    double binHz(int k, double sampleRate) const { return k * sampleRate / size_; }

    // Fn: void(std::complex<double>* bins, int count), count = bins().
    template <class Fn>
    void process(int channel, double* x, int frames, Fn&& spectrum) {
        Lane& lane = lanes_[static_cast<size_t>(channel)];
        const size_t n = static_cast<size_t>(size_);
        for (int i = 0; i < frames; ++i) {
            lane.input[lane.position] = x[i];
            if (--lane.untilFrame == 0) {
                lane.untilFrame = hop_;
                transform(lane, spectrum);
            }
            // The frame just made covers this sample and the size - 1 after
            // it; what has accumulated here is complete.
            x[i] = lane.output[lane.position];
            lane.output[lane.position] = 0.0;
            lane.position = (lane.position + 1) % n;
        }
    }

  private:
    struct Lane {
        std::vector<double> input, output;  // rings, both at `position`
        size_t position = 0;
        int untilFrame = 1;
    };

    template <class Fn>
    void transform(Lane& lane, Fn& spectrum) {
        const size_t n = static_cast<size_t>(size_);
        // The newest `size` samples, oldest first, windowed.
        const size_t oldest = (lane.position + 1) % n;
        for (size_t k = 0; k < n; ++k) frame_[k] = lane.input[(oldest + k) % n] * window_[k];
        fft_.forward(frame_.data());
        spectrum(frame_.data(), bins());
        frame_[0] = frame_[0].real();
        frame_[n / 2] = frame_[n / 2].real();
        for (size_t k = 1; k < n / 2; ++k) frame_[n - k] = std::conj(frame_[k]);
        fft_.inverse(frame_.data());
        // Sample k of the frame was input `size - 1 - k` samples ago; it
        // plays `latency()` after that, k samples from now.
        for (size_t k = 0; k < n; ++k) lane.output[(lane.position + k) % n] += frame_[k].real() * window_[k] * scale_;
    }

    Fft fft_{16};
    std::vector<double> window_;
    std::vector<std::complex<double>> frame_;
    std::vector<Lane> lanes_;
    int size_ = 16, overlap_ = 4, hop_ = 4;
    double scale_ = 1.0;
};

}  // namespace supple::dsp
