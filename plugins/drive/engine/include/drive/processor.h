// SPDX-License-Identifier: MIT
#pragma once
#include "drive/params.h"
#include "supple/dsp/oversampler.h"
namespace supple::drive {
// Odd unity-slope curves and their even antiderivatives (F(0) = 0).
double curveValue(int curve, double x) SUPPLE_RT;
double curveIntegral(int curve, double x) SUPPLE_RT;
double curveAdaa(int curve, double x, double previous) SUPPLE_RT;
class Processor {
  public:
    void prepare(double sampleRate, uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(size_t index, double physical) SUPPLE_RT;
    void setParams(const Values&) SUPPLE_RT;
    uint32_t latency() const { return oversamplers_[0].latency() + 1; }
    int latencySamples() const { return static_cast<int>(latency()); }
    int factor() const { return factor_; }
    double compensation(int curve, double driveDb) const SUPPLE_RT;
    struct Telemetry {
        double inputPeakDb, outputPeakDb;
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
        void set(double v, int samples, bool snap) SUPPLE_RT;
        double next() SUPPLE_RT;
    };
    struct Tilt {
        double previousX = 0, previousY = 0;
        double process(double x, double b0, double b1, double a1) SUPPLE_RT;
    };
    template <class T>
    void run(const T* const* in, T* const* out, uint32_t frames, bool telemetry) SUPPLE_RT;
    void configure(bool snap) SUPPLE_RT;
    Values values_ = defaults();
    std::array<Ramp, 6> weights_{};
    Ramp drive_, bias_, tone_, autoGain_, mix_, output_;
    std::array<std::array<double, 145>, 6> gainTable_{};
    std::array<dsp::Oversampler, 2> oversamplers_{};
    std::array<std::array<double, 1024>, 2> dry_{};
    std::array<std::array<double, 8>, 2> wetPad_{};
    Tilt pre_[2], post_[2];
    double previous_[2]{}, adaaPrevious_[2]{}, dcX_[2]{}, dcY_[2]{};
    double sr_ = 48000, dcAlpha_ = 0, inputPeak_ = 0, outputPeak_ = 0;
    size_t dryHead_ = 0, padHead_ = 0;
    int channels_ = 2, factor_ = 4;
    bool started_ = false;
};
}  // namespace supple::drive
