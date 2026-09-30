#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "supple/dsp/dsp.h"
#include "transient/params.h"
namespace supple::transient {
class Processor {
  public:
    struct Telemetry {
        double inputPeakDb = -120, outputPeakDb = -120, gain = 0;
    };
    Processor();
    Status prepare(double sampleRate, std::uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(std::size_t, double physical) SUPPLE_RT;
    std::uint32_t latency() const SUPPLE_RT { return latencyFor(target_); }
    double shapingGain() const SUPPLE_RT { return gainDb_; }
    Telemetry takeTelemetry() SUPPLE_RT;
    template <class T>
    void process(const T* const* input, T* const* output, std::uint32_t frames) SUPPLE_RT {
        dsp::DenormalGuard guard;
        if (!ready_ || !output) return;
        started_ = true;
        for (std::uint32_t i = 0; i < frames; ++i) {
            std::array<double, 2> x{}, y{};
            for (int c = 0; c < channels_; ++c) {
                x[c] = input && input[c] ? static_cast<double>(input[c][i]) : 0;
                if (!std::isfinite(x[c])) x[c] = 0;
            }
            if (channels_ == 1) x[1] = x[0];
            tick(x, y);
            const bool neutral = target_[kClip] < .5 && gainDb_ == 0 && current_[kOutput] == 0;
            for (int c = 0; c < channels_; ++c) {
                if (!output[c]) continue;
                // Copy the original finite sample when neutral: converting or
                // multiplying a subnormal under DenormalGuard can flush it.
                if (neutral && input && input[c] && std::isfinite(input[c][i]))
                    output[c][i] = input[c][i];
                else
                    output[c][i] = static_cast<T>(y[c]);
            }
        }
    }

  private:
    // Local 2x half-band pair: 257-tap Kaiser(beta=11) windowed sinc.
    // Each FIR has 128 high-rate samples delay; the pair is 128 base samples.
    static constexpr std::size_t kTaps = 257;
    struct Fir {
        std::array<double, kTaps> history{};
        std::size_t head = 0;
        double run(double, const std::array<double, kTaps>&) SUPPLE_RT;
    };
    struct Peak {
        std::uint64_t time = 0;
        double value = 0;
    };
    void tick(const std::array<double, 2>&, std::array<double, 2>&) SUPPLE_RT;
    double detect(double) SUPPLE_RT;
    Values target_, current_;
    std::array<double, kTaps> taps_{};
    std::array<Fir, 2> up_{}, down_{};
    std::array<std::array<double, kClipLatency + 1>, 2> dry_{};
    std::vector<Peak> peaks_;
    std::size_t read_ = 0, write_ = 0, dryHead_ = 0;
    std::uint64_t clock_ = 0;
    std::uint32_t hold_ = 48, minHold_ = 48, maxHold_ = 2400;
    std::uint64_t lastPeak_ = 0;
    double previous_ = 0, previous2_ = 0;
    bool havePeak_ = false;
    double sampleRate_ = 48000, smooth_ = 0, gainAlpha_ = 0, fast_ = 0, slow_ = 0, gainDb_ = 0,
           reference_ = 0;
    double fastAttack_ = 0, fastRelease_ = 0, slowRelease_ = 0, lastSpeed_ = 0;
    double inputPeak_ = 0, outputPeak_ = 0, gainSum_ = 0;
    std::uint64_t meterSamples_ = 0;
    int channels_ = 2;
    bool ready_ = false, started_ = false, detectorStarted_ = false;
};
}  // namespace supple::transient
