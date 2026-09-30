// SPDX-License-Identifier: MIT
#pragma once
#include <vector>
#include "gate/params.h"
namespace supple::gate {
struct Telemetry {
    double level = -120, gr = -80;
    bool open = false;
};
class Processor {
  public:
    void prepare(double sampleRate, uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(size_t index, double physical) SUPPLE_RT;
    void setParams(std::span<const double>) SUPPLE_RT;
    uint32_t latency() const { return delaySamples_; }
    int latencySamples() const { return static_cast<int>(latency()); }
    Telemetry telemetry() const SUPPLE_RT;
    // External key is optional stereo, even for a mono main bus; missing key is silence.
    void process(const float* const* in, float* const* out, uint32_t frames,
                 const float* const* key = nullptr) SUPPLE_RT;
    void process(const double* const* in, double* const* out, uint32_t frames,
                 const double* const* key = nullptr) SUPPLE_RT;
    void process(float* const* io, int frames, bool = false) SUPPLE_RT;
    void process(double* const* io, int frames, bool = false) SUPPLE_RT;

  private:
    struct Ramp {
        double value = 0, target = 0, step = 0;
        uint32_t left = 0;
        void set(double, uint32_t, bool);
        double next();
    };
    struct Filter {
        double a = 0, b = 0;
        double run(double, double, bool);
    };
    struct Channel {
        bool open = false;
        uint32_t hold = 0, deadline = 0;
        double gate = -80, expand = -80, gateStart = -80, expandStart = -80;
    };
    template <class T>
    void run(const T* const*, T* const*, uint32_t, const T* const*);
    void configure(bool snap);
    Values values_ = defaults();
    std::array<Ramp, kNumParams> ramp_{};
    Ramp hpMix_, lpMix_;
    Filter hp_[2], lp_[2];
    Channel channel_[2];
    std::vector<std::array<double, 4>> delay_;
    size_t write_ = 0;
    uint32_t delaySamples_ = 0, grid_ = 0;
    double sr_ = 48000, hpG_ = 0, lpG_ = 0, attack_ = 0, release_ = 0;
    double level_ = -120, gr_ = -80;
    int channels_ = 2;
    bool started_ = false, open_ = false;
};
}  // namespace supple::gate
