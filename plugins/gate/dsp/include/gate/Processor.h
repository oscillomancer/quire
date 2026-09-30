// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Gate DSP core -- intentionally independent of every plug-in SDK.
#pragma once

#include <algorithm>
#include <cmath>

namespace gate {

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g) { return 20.0 * std::log10(std::max(g, 1.0e-12)); }

class Processor {
  public:
    struct Params {
        double inputGainDb = 0.0;
        double outputGainDb = 0.0;
        double mix = 1.0;
        bool bypass = false;
    };

    struct Telemetry {
        double inputPeakDb = -120.0;
        double outputPeakDb = -120.0;
    };

    void prepare(double sampleRate, int channels) {
        sampleRate_ = std::max(1.0, sampleRate);
        channels_ = std::max(1, channels);
        input_.setTime(15.0, sampleRate_);
        output_.setTime(15.0, sampleRate_);
        mix_.setTime(15.0, sampleRate_);
        bypass_.setTime(5.0, sampleRate_);
        needsInitialSnap_ = true;
        resetMeters();
    }

    // The first parameter set after prepare/reset is snapped into place. This
    // avoids a startup ramp from constructor defaults when a session restores
    // non-default values. Later updates use the normal click-free smoothing.
    void setParams(const Params& p) {
        params_ = p;
        const double in = dbToGain(std::clamp(p.inputGainDb, -60.0, 24.0));
        const double out = dbToGain(std::clamp(p.outputGainDb, -60.0, 24.0));
        const double mix = std::clamp(p.mix, 0.0, 1.0);
        const double bypass = p.bypass ? 1.0 : 0.0;
        if (needsInitialSnap_) {
            input_.reset(in);
            output_.reset(out);
            mix_.reset(mix);
            bypass_.reset(bypass);
            needsInitialSnap_ = false;
        } else {
            input_.target = in;
            output_.target = out;
            mix_.target = mix;
            bypass_.target = bypass;
        }
    }

    void reset() {
        // Reset history while preserving the current session state exactly.
        needsInitialSnap_ = true;
        setParams(params_);
        resetMeters();
    }

    int latencySamples() const { return 0; }

    template <typename T>
    void process(T* const* io, int numSamples, bool captureMeters = false) {
        if (!io || numSamples <= 0) return;
        double inPeak = 0.0;
        double outPeak = 0.0;

        for (int i = 0; i < numSamples; ++i) {
            const double inGain = input_.next();
            const double outGain = output_.next();
            const double wetMix = mix_.next();
            const double bypassMix = bypass_.next();
            for (int c = 0; c < channels_; ++c) {
                if (!io[c]) continue;
                const double dry = static_cast<double>(io[c][i]);
                const double driven = dry * inGain;
                const double effected = processSample(driven, c);
                const double wet = effected * outGain;
                const double active = dry + (wet - dry) * wetMix;
                const double y = active + (dry - active) * bypassMix;
                io[c][i] = static_cast<T>(y);
                if (captureMeters) {
                    inPeak = std::max(inPeak, std::abs(dry));
                    outPeak = std::max(outPeak, std::abs(y));
                }
            }
        }

        if (captureMeters) {
            inputPeak_ = std::max(inputPeak_, inPeak);
            outputPeak_ = std::max(outputPeak_, outPeak);
        }
    }

    Telemetry takeTelemetry() {
        Telemetry t{gainToDb(inputPeak_), gainToDb(outputPeak_)};
        resetMeters();
        return t;
    }

  private:
    struct Smoother {
        double current = 1.0;
        double target = 1.0;
        double coefficient = 0.0;

        void setTime(double ms, double sr) {
            coefficient = std::exp(-1.0 / (std::max(0.01, ms) * 0.001 * sr));
        }
        void reset(double v) { current = target = v; }
        double next() {
            current = target + coefficient * (current - target);
            return current;
        }
    };

    // Replace this identity operation with the product DSP. Keep its state in
    // Processor, size any scratch storage in prepare(), and never allocate,
    // lock, log, or perform I/O from this call chain.
    double processSample(double x, int channel) {
        (void)channel;
        return x;
    }

    void resetMeters() { inputPeak_ = outputPeak_ = 0.0; }

    Params params_{};
    double sampleRate_ = 48000.0;
    int channels_ = 2;
    bool needsInitialSnap_ = true;
    Smoother input_, output_, mix_, bypass_;
    double inputPeak_ = 0.0;
    double outputPeak_ = 0.0;
};

}  // namespace gate
