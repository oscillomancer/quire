// SPDX-License-Identifier: MIT
#pragma once
#include "supple/base/rt.h"
#include "utility/params.h"
namespace supple::utility {
class Processor {
  public:
    void prepare(double sampleRate, uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(size_t index, double physical) SUPPLE_RT;
    void setParams(const Values&) SUPPLE_RT;
    uint32_t latency() const { return 0; }
    int latencySamples() const { return 0; }
    struct Telemetry {
        double inputPeakDb, outputPeakDb, correlation;
    };
    Telemetry takeTelemetry() SUPPLE_RT;
    void process(const float* const* in, float* const* out, uint32_t frames) SUPPLE_RT;
    void process(const double* const* in, double* const* out, uint32_t frames) SUPPLE_RT;
    void process(float* const* io, int frames, bool telemetry = false) SUPPLE_RT;
    void process(double* const* io, int frames, bool telemetry = false) SUPPLE_RT;

  private:
    struct Ramp {
        double value = 0, target = 0, step = 0;
        int remaining = 0;
        void set(double v, int samples, bool snap);
        double next();
    };
    struct Filter {
        double a = 0, b = 0;
        double highpass(double x, double g);
    };
    template <class T>
    void run(const T* const* in, T* const* out, uint32_t frames, bool telemetry);
    void configure(bool snap);
    Values values_ = defaults();
    std::array<Ramp, 4> matrix_{};
    Ramp gain_, pan_, width_, bass_, bassMix_, dcMix_, mute_, invertL_, invertR_;
    Filter side_[2];
    double dcX_[2]{}, dcY_[2]{};
    double sr_ = 48000, dcAlpha_ = 0;
    double inputPeak_ = 0, outputPeak_ = 0, ll_ = 0, rr_ = 0, lr_ = 0;
    int channels_ = 2;
    bool started_ = false;
};
}  // namespace supple::utility
