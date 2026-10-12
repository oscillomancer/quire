// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: a ducker keyed from the sidechain. While the key's peak
// is above the threshold the input is turned down by Depth, reaching it over
// Attack and returning over Release. One gain for every channel, so it means
// the same in mono, stereo, 5.1, 7.1.4 and ambisonics.
//
//   - the sidechain arrives in Block::sidechain, as wide as the main bus,
//     silence when the host connected nothing (so nothing is ducked);
//   - the aux output, Block::aux(0), carries what was ducked away: the input
//     times one minus the gain, so main plus aux is the input;
//   - the gain lands exactly on 1 and on Depth, so an idle key leaves the
//     input bit-exact;
//   - Reduction (an output parameter) reports the deepest gain of each block.
#pragma once

#include <algorithm>
#include <cmath>

#include "core/Dsp.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    // What the editor draws: the key's peak and the gain, each block.
    struct Telemetry {
        float key = 0.0f;   // peak of the sidechain since the last frame
        float gain = 1.0f;  // the lowest gain since the last frame
    };
    static constexpr double kTelemetryHz = 60.0;
    // The editor draws the input and output as they scroll by.
    static constexpr bool kAudioFeed = true;

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        reset();
    }

    void reset() {
        gain_ = 1.0;
        key_ = 0.0f;
        lowest_ = 1.0f;
    }

    void update(const supple::ParamValues& v) {
        threshold_ = supple::dsp::dbToGain(v[kThreshold]);
        depth_ = v.gain(kDepth);
        attack_ = coefficient(v[kAttack]);
        release_ = coefficient(v[kRelease]);
    }

    void process(supple::Block& b) {
        double lowest = gain_;
        for (int i = 0; i < b.frames; ++i) {
            double key = 0.0;
            for (int c = 0; c < b.channels; ++c) key = std::max(key, std::fabs(b.sidechain[c][i]));
            if (b.metering) key_ = std::max(key_, static_cast<float>(key));
            const double target = key > threshold_ ? depth_ : 1.0;
            gain_ += (target - gain_) * (target < gain_ ? attack_ : release_);
            if (std::fabs(target - gain_) < 1e-9) gain_ = target;  // land exactly
            lowest = std::min(lowest, gain_);
            for (int c = 0; c < b.channels; ++c) {
                const double x = b.channel(c)[i];
                b.channel(c)[i] = x * gain_;
                b.aux(0)[c][i] = x * (1.0 - gain_);
            }
        }
        if (b.metering) lowest_ = std::min(lowest_, static_cast<float>(lowest));
        setOutput(kReduction, std::max(-60.0, supple::dsp::gainToDb(lowest, -60.0)));
    }

    void captureTelemetry(Telemetry& t) {
        t.key = key_;
        t.gain = lowest_;
        key_ = 0.0f;
        lowest_ = static_cast<float>(gain_);
    }

    double gain() const { return gain_; }

  private:
    // A one-pole step reaching 1 - 1/e of the way in `ms`.
    double coefficient(double ms) const { return 1.0 - std::exp(-1.0 / std::max(1.0, ms * 0.001 * sampleRate_)); }

    double sampleRate_ = 48000.0;
    double threshold_ = 0.03, depth_ = 0.25, attack_ = 0.1, release_ = 0.001;
    double gain_ = 1.0;
    float key_ = 0.0f, lowest_ = 1.0f;
};

}  // namespace product
