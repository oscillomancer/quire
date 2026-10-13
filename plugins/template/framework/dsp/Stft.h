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
// Or a sample at a time, without a callback (for code that has none, such
// as Quire's DSP scripts):
//
//   if (stft_.push(c, x)) change(stft_.spectrum(c), stft_.bins());
//   y = stft_.pull(c);
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
        lanes_.assign(static_cast<size_t>(std::max(0, channels)), Lane{});
        for (Lane& lane : lanes_) {
            lane.input.assign(static_cast<size_t>(size_), 0.0);
            lane.output.assign(static_cast<size_t>(size_), 0.0);
            lane.frame.assign(static_cast<size_t>(size_), {});
        }
        reset();
    }
    void reset() {
        for (Lane& lane : lanes_) {
            std::fill(lane.input.begin(), lane.input.end(), 0.0);
            std::fill(lane.output.begin(), lane.output.end(), 0.0);
            std::fill(lane.frame.begin(), lane.frame.end(), std::complex<double>{});
            lane.position = 0;
            lane.untilFrame = hop_;
            lane.pushed = lane.pending = false;
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
        for (int i = 0; i < frames; ++i) {
            if (push(channel, x[i])) spectrum(this->spectrum(channel), bins());
            x[i] = pull(channel);
        }
    }

    // A sample at a time: push() the input; when it returns true, a frame's
    // spectrum is ready in spectrum(channel), bins 0 to bins() - 1, to read
    // and change before pull() gives the output sample. One push() and one
    // pull() per sample: a push() without a pull() drops that sample's
    // output, and a pull() without a push() pushes silence.
    bool push(int channel, double x) {
        Lane& lane = lanes_[static_cast<size_t>(channel)];
        if (lane.pushed) pull(channel);
        lane.input[lane.position] = x;
        lane.pushed = true;
        if (--lane.untilFrame == 0) {
            lane.untilFrame = hop_;
            analyse(lane);
            lane.pending = true;
        }
        return lane.pending;
    }
    std::complex<double>* spectrum(int channel) { return lanes_[static_cast<size_t>(channel)].frame.data(); }
    double pull(int channel) {
        Lane& lane = lanes_[static_cast<size_t>(channel)];
        if (!lane.pushed) push(channel, 0.0);
        if (lane.pending) {
            synthesise(lane);
            lane.pending = false;
        }
        // The frame just made covers this sample and the size - 1 after it;
        // what has accumulated here is complete.
        const double y = lane.output[lane.position];
        lane.output[lane.position] = 0.0;
        lane.position = (lane.position + 1) % lane.output.size();
        lane.pushed = false;
        return y;
    }

  private:
    struct Lane {
        std::vector<double> input, output;  // rings, both at `position`
        std::vector<std::complex<double>> frame;
        size_t position = 0;
        int untilFrame = 1;
        bool pushed = false;   // this sample's input is in, its output not yet out
        bool pending = false;  // a frame is analysed and not yet added back
    };

    void analyse(Lane& lane) const {
        const size_t n = static_cast<size_t>(size_);
        // The newest `size` samples, oldest first, windowed.
        const size_t oldest = (lane.position + 1) % n;
        for (size_t k = 0; k < n; ++k) lane.frame[k] = lane.input[(oldest + k) % n] * window_[k];
        fft_.forward(lane.frame.data());
    }

    void synthesise(Lane& lane) const {
        const size_t n = static_cast<size_t>(size_);
        std::vector<std::complex<double>>& frame = lane.frame;
        frame[0] = frame[0].real();
        frame[n / 2] = frame[n / 2].real();
        for (size_t k = 1; k < n / 2; ++k) frame[n - k] = std::conj(frame[k]);
        fft_.inverse(frame.data());
        // Sample k of the frame was input `size - 1 - k` samples ago; it
        // plays `latency()` after that, k samples from now.
        for (size_t k = 0; k < n; ++k) lane.output[(lane.position + k) % n] += frame[k].real() * window_[k] * scale_;
    }

    Fft fft_{16};
    std::vector<double> window_;
    std::vector<Lane> lanes_;
    int size_ = 16, overlap_ = 4, hop_ = 4;
    double scale_ = 1.0;
};

}  // namespace supple::dsp
