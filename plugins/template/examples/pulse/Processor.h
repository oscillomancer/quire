// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: a tremolo whose gain follows a low-frequency oscillator,
// and an auto-pan when the right channel's oscillator runs half a cycle
// behind the left's (Stereo Phase 180°).
//
// Points worth copying:
//   - Depth, Stereo Phase and Output are smoothed by the framework: marked
//     .smoothed(ms) in Params.h and read per sample from Block::smoothed.
//     Nothing here keeps a Smoother.
//   - Synced and with the host playing, the oscillator's phase is computed
//     from the song position, so a pulse lands on the same beat wherever
//     playback starts. Stopped, or free, it runs on its own.
//   - The output parameter Gain is reported with setOutput(); the framework
//     passes it to the host and the editor.
//   - Output is off at its minimum: levelGain() is exactly 0 there.
//   - At Depth 0 and Output 0 dB the output is the input, bit for bit.
#pragma once

#include <cmath>

#include "core/Dsp.h"
#include "core/ParamTable.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    enum Shape { kSine, kTriangle, kSquare };

    // The oscillator, 1 at phase 0 and 0 half a cycle later.
    static double shapeAt(int shape, double phase) {
        const double c = std::cos(2.0 * supple::dsp::kPi * phase);
        switch (shape) {
            case kTriangle: return std::fabs(1.0 - 2.0 * phase);
            case kSquare: return std::clamp(0.5 + 3.0 * c, 0.0, 1.0);  // edges a twentieth of a cycle long
            default: return 0.5 + 0.5 * c;
        }
    }
    // Depth 0 is exactly unity, whatever the oscillator.
    static double tremolo(double depth, double shape) { return 1.0 - depth * (1.0 - shape); }

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) { sampleRate_ = setup.sampleRate; }

    void reset() { phase_ = 0.0; }

    void update(const supple::ParamValues& v) {
        sync_ = v.on(kSync);
        rateHz_ = v[kRate];
        beats_ = v.beats(kDivision);
        shape_ = v.choice(kShape);
    }

    void process(supple::Block& b) {
        const double* depth = b.smoothed(kDepth);
        const double* stereo = b.smoothed(kPhase);
        const double* output = b.smoothed(kOutput);
        const bool outputMoving = b.gliding(kOutput);
        const double steadyOutput = supple::levelGain(kOutputDef, output[0]);
        const supple::Transport* t = b.transport;
        if (sync_ && t && t->playing && t->hasTempo && t->hasBeats) phase_ = wrap(t->beats / beats_);
        const double step = sync_ ? b.tempo() / 60.0 / beats_ / sampleRate_ : rateHz_ / sampleRate_;
        double left = 1.0;
        for (int i = 0; i < b.frames; ++i) {
            const double out = outputMoving ? supple::levelGain(kOutputDef, output[i]) : steadyOutput;
            left = tremolo(depth[i], shapeAt(shape_, phase_));
            b.channel(0)[i] *= left * out;
            if (b.channels > 1) {
                const double right = tremolo(depth[i], shapeAt(shape_, wrap(phase_ + stereo[i] / 360.0)));
                for (int c = 1; c < b.channels; ++c) b.channel(c)[i] *= right * out;
            }
            phase_ = wrap(phase_ + step);
        }
        setOutput(kGain, left);
    }

  private:
    static constexpr const supple::ParamDef& kOutputDef = kParams[supple::params::indexOf(kOutput)];

    static double wrap(double x) { return x - std::floor(x); }

    double sampleRate_ = 48000.0;
    double phase_ = 0.0;  // cycles, 0..1
    double rateHz_ = 4.0;
    double beats_ = 0.5;
    int shape_ = kSine;
    bool sync_ = true;
};

}  // namespace product
