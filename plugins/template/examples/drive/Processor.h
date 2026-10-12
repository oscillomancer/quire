// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: a saturator, kept small, because what this example
// shows is the editor it does not have (see product.cmake) -- and how a
// nonlinearity is kept clean with the DSP library:
//
//   - Drive into one of three shapes: tanh, a hard clip at full scale, or
//     a fold that reflects whatever passes full scale back inside it;
//   - the shaper runs oversampled (Quality, 2x to 8x; dsp/Oversampler.h,
//     linear phase), so harmonics above Nyquist are filtered away instead of
//     folding back as inharmonic aliases. Its latency is reported, the dry
//     path is delayed to match, and changing Quality asks the host to
//     restart, since latency may only change then;
//   - the soft and hard shapes add antiderivative anti-aliasing on top
//     (dsp/Shapers.h): most of what aliasing is left, gone for a few
//     operations;
//   - a one-pole low-pass after the shaper (Tone), a mix with the dry
//     input, and the output level, each smoothed(20) by the framework;
//   - two output parameters, read the way broadcast meters read: Peak, the
//     true peak (between samples too), falling back at 20 dB a second; and
//     Loudness, the short-term loudness in LUFS (dsp/Loudness.h).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Dsp.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "dsp/Dynamics.h"
#include "dsp/Loudness.h"
#include "dsp/Oversampler.h"
#include "dsp/Shapers.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    static int factorFor(int quality) { return 1 << std::clamp(quality, 0, 3); }

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues& v) {
        sampleRate_ = setup.sampleRate;
        const size_t channels = static_cast<size_t>(setup.channels);
        over_.prepare(setup.channels, setup.maxBlockSize, factorFor(v.choice(kQuality)));
        dryDelay_.assign(channels, {});
        for (auto& d : dryDelay_) d.prepare(over_.latency());
        dry_.assign(static_cast<size_t>(setup.maxBlockSize), 0.0);
        lowpass_.assign(channels, 0.0);
        soft_.assign(channels, {});
        hard_.assign(channels, {});
        truePeak_.assign(channels, {});
        loudness_.prepare(sampleRate_, setup.channels);
        // 20 dB a second, per sample.
        fall_ = std::pow(10.0, -20.0 / 20.0 / sampleRate_);
        reset();
    }

    // The oversampler's, fixed until the next prepare(): Quality restarts.
    int latencySamples() const { return over_.latency(); }

    void reset() {
        over_.reset();
        for (auto& d : dryDelay_) d.clear();
        std::fill(lowpass_.begin(), lowpass_.end(), 0.0);
        for (auto& s : soft_) s = {};
        for (auto& s : hard_) s = {};
        for (auto& s : truePeak_) s = {};
        loudness_.reset();
        peak_ = 0.0;
    }

    void update(const supple::ParamValues& v) { shape_ = v.choice(kShape); }

    // The shapes, plainly, on a sample already driven: each bounded by 1.
    static double shaped(int shape, double x) {
        switch (shape) {
            case 1: return std::clamp(x, -1.0, 1.0);
            case 2: {
                // A triangle through the origin with period 4: identity on
                // [-1, 1], reflected beyond it.
                const double t = std::fmod(std::fabs(x) + 1.0, 4.0);
                const double folded = t < 2.0 ? t - 1.0 : 3.0 - t;
                return x < 0.0 ? -folded : folded;
            }
            default: return std::tanh(x);
        }
    }

    void process(supple::Block& b) {
        using supple::dsp::Adaa;
        const double* drive = b.smoothed(kDrive);
        const double* tone = b.smoothed(kTone);
        const double* mix = b.smoothed(kMix);
        const double* output = b.smoothed(kOutput);
        const double twoPiOverRate = 2.0 * supple::dsp::kPi / sampleRate_;
        const int factor = over_.factor(), latency = over_.latency();
        for (int c = 0; c < b.channels; ++c) {
            const size_t k = static_cast<size_t>(c);
            double* x = b.channel(c);
            for (int i = 0; i < b.frames; ++i) dry_[static_cast<size_t>(i)] = dryDelay_[k].process(x[i], latency);
            double* hi = over_.up(c, x, b.frames);
            for (int i = 0; i < b.frames; ++i) {
                const double gain = supple::dsp::dbToGain(drive[i]);
                double* run = hi + static_cast<ptrdiff_t>(i) * factor;
                for (int j = 0; j < factor; ++j) {
                    const double v = run[j] * gain;
                    run[j] = shape_ == 0   ? Adaa<supple::dsp::TanhShape>::tick(soft_[k], v)
                             : shape_ == 1 ? Adaa<supple::dsp::HardClipShape>::tick(hard_[k], v)
                                           : shaped(2, v);
                }
            }
            over_.down(c, x, b.frames);
            double& state = lowpass_[k];
            for (int i = 0; i < b.frames; ++i) {
                const double dry = dry_[static_cast<size_t>(i)];
                state += (x[i] - state) * (1.0 - std::exp(-twoPiOverRate * tone[i]));
                x[i] = (dry + (state - dry) * mix[i]) * supple::dsp::dbToGain(output[i]);
            }
        }
        // What came out, metered: the loudest point, falling back between
        // peaks, and the loudness.
        for (int i = 0; i < b.frames; ++i) {
            double loudest = 0.0;
            for (int c = 0; c < b.channels; ++c) loudest = std::max(loudest, meter_.tick(truePeak_[static_cast<size_t>(c)], b.channel(c)[i]));
            peak_ = std::max(loudest, peak_ * fall_);
        }
        const double* channels[16] = {};
        for (int c = 0; c < std::min(b.channels, 16); ++c) channels[c] = b.channel(c);
        loudness_.process(channels, b.frames);
        setOutput(kPeak, std::clamp(supple::dsp::gainToDb(peak_, -60.0), -60.0, 6.0));
        setOutput(kLoudness, std::clamp(loudness_.shortTerm(), -60.0, 0.0));
    }

  private:
    double sampleRate_ = 48000.0;
    int shape_ = 0;
    supple::dsp::Oversampler over_;
    std::vector<supple::dsp::DelayLine> dryDelay_;
    std::vector<double> dry_;
    std::vector<double> lowpass_;
    std::vector<supple::dsp::Adaa<supple::dsp::TanhShape>::State> soft_;
    std::vector<supple::dsp::Adaa<supple::dsp::HardClipShape>::State> hard_;
    supple::dsp::TruePeak meter_;
    std::vector<supple::dsp::TruePeak::State> truePeak_;
    supple::dsp::Loudness loudness_;
    double peak_ = 0.0, fall_ = 1.0;
};

}  // namespace product
