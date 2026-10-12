// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP. Replace this with the real thing; keep the shape.
//
// The starter is a utility filter: input gain, a low cut and a high cut
// (12 or 24 dB/oct Butterworth), dry/wet mix and output gain. At its defaults
// both filters are switched off, the gains are unity and the mix is fully
// wet, so the output is bit-identical to the input -- which keeps null tests
// meaningful while the product is still being built.
//
// Points worth copying:
//   - every audible parameter is smoothed per sample, so output does not
//     depend on where the host splits blocks;
//   - toggles and the slope crossfade instead of switching, so nothing clicks;
//   - the filters keep running while switched off, so switching them on
//     starts from a settled state;
//   - nothing allocates outside prepare();
//   - kAudioFeed hands the editor the audio for its spectrum analyser.
#pragma once

#include <cmath>
#include <vector>

#include "core/Dsp.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    // Butterworth pole quality factors.
    static constexpr double kQ2 = 0.70710678118654752;         // 2nd order
    static constexpr double kQ4a = 0.54119610014619698;        // 4th order, first pair
    static constexpr double kQ4b = 1.30656296487637652;        // 4th order, second pair
    // The editor draws the output's spectrum behind the response curve.
    static constexpr bool kAudioFeed = true;

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        inputGain_.setTime(20.0, sampleRate_);
        outputGain_.setTime(20.0, sampleRate_);
        lowCut_.setTime(30.0, sampleRate_);
        highCut_.setTime(30.0, sampleRate_);
        lowOn_.setTime(15.0, sampleRate_);
        highOn_.setTime(15.0, sampleRate_);
        steep_.setTime(20.0, sampleRate_);
        channels_.assign(static_cast<size_t>(setup.channels), Channel{});
        dryWet_.prepare(setup, maxLatencySamples());
    }

    void reset() {
        for (auto& c : channels_) c = Channel{};
        dryWet_.reset();
    }

    void update(const supple::ParamValues& v) {
        const bool snap = v.initial();
        inputGain_.set(supple::dsp::dbToGain(v[kInputGain]), snap);
        outputGain_.set(supple::dsp::dbToGain(v[kOutputGain]), snap);
        // Cutoffs glide in log frequency: the same speed per octave anywhere.
        lowCut_.set(std::log(v[kLowCut]), snap);
        highCut_.set(std::log(v[kHighCut]), snap);
        lowOn_.set(v.on(kLowCutOn) ? 1.0 : 0.0, snap);
        highOn_.set(v.on(kHighCutOn) ? 1.0 : 0.0, snap);
        steep_.set(v.choice(kSlope) == 1 ? 1.0 : 0.0, snap);
        dryWet_.setMix(v[kMix], snap);
        if (snap) coefficientsStale_ = true;
    }

    void process(supple::Block& b) {
        dryWet_.capture(b);
        for (int i = 0; i < b.frames; ++i) {
            if (coefficientsStale_ || !lowCut_.settled() || !highCut_.settled()) {
                setCoefficients(std::exp(lowCut_.next()), std::exp(highCut_.next()));
                coefficientsStale_ = false;
            }
            const double gain = inputGain_.next();
            const double low = lowOn_.next();
            const double high = highOn_.next();
            const double steep = steep_.next();
            for (int c = 0; c < b.channels; ++c) {
                Channel& ch = channels_[static_cast<size_t>(c)];
                double x = b.channel(c)[i] * gain;
                const double hp2 = low2_.tick(ch.low2, x).high;
                const double hp4 = low4b_.tick(ch.low4b, low4a_.tick(ch.low4a, x).high).high;
                x += (hp2 + (hp4 - hp2) * steep - x) * low;
                const double lp2 = high2_.tick(ch.high2, x).low;
                const double lp4 = high4b_.tick(ch.high4b, high4a_.tick(ch.high4a, x).low).low;
                x += (lp2 + (lp4 - lp2) * steep - x) * high;
                b.channel(c)[i] = x;
            }
        }
        dryWet_.apply(b);
        for (int i = 0; i < b.frames; ++i) {
            const double gain = outputGain_.next();
            for (int c = 0; c < b.channels; ++c) b.channel(c)[i] *= gain;
        }
    }

  private:
    struct Channel {
        supple::dsp::Svf::State low2, low4a, low4b, high2, high4a, high4b;
    };

    void setCoefficients(double lowHz, double highHz) {
        low2_.setCoefficients(lowHz, kQ2, sampleRate_);
        low4a_.setCoefficients(lowHz, kQ4a, sampleRate_);
        low4b_.setCoefficients(lowHz, kQ4b, sampleRate_);
        high2_.setCoefficients(highHz, kQ2, sampleRate_);
        high4a_.setCoefficients(highHz, kQ4a, sampleRate_);
        high4b_.setCoefficients(highHz, kQ4b, sampleRate_);
    }

    double sampleRate_ = 48000.0;
    supple::dsp::Smoother inputGain_, outputGain_, lowCut_, highCut_;
    supple::dsp::Ramp lowOn_, highOn_, steep_;
    supple::dsp::Svf low2_, low4a_, low4b_, high2_, high4a_, high4b_;
    supple::dsp::DryWet dryWet_;
    std::vector<Channel> channels_;
    bool coefficientsStale_ = true;
};

}  // namespace product
