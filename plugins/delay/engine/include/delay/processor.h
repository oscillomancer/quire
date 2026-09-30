// SPDX-License-Identifier: MIT
#pragma once
#include <vector>
#include "delay/params.h"
namespace supple::delay {
struct Telemetry {
    std::array<double, 2> timeMs{375, 375};
    double tempo = 120, inputDb = -120, wetDb = -120, duckDb = 0;
};
class Processor {
  public:
    bool prepare(double sampleRate, uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(size_t index, double physical) SUPPLE_RT;
    void setParams(std::span<const double>) SUPPLE_RT;
    void setTempo(double bpm) SUPPLE_RT;
    void process(const float* const* in, float* const* out, uint32_t frames) SUPPLE_RT;
    void process(const double* const* in, double* const* out, uint32_t frames) SUPPLE_RT;
    uint32_t latency() const { return 0; }
    Telemetry telemetry() const SUPPLE_RT { return telemetry_; }

  private:
    struct Ramp {
        double value = 0, target = 0, step = 0;
        uint32_t left = 0;
        void set(double, uint32_t) SUPPLE_RT;
        double next() SUPPLE_RT;
    };
    struct Filter {
        double s1 = 0, s2 = 0, g = 0, a1 = 1, a2 = 0, a3 = 0;
        void tune(double hz, double fs) SUPPLE_RT;
        double tick(double x, bool high) SUPPLE_RT;
    };
    struct Line {
        std::vector<float> buffer;
        size_t write = 0, frozenStart = 0, frozenLength = 0, frozenPos = 0;
        double glide1 = 0, glide2 = 0, frozenFraction = 0;
        double randomA = 0, randomB = 0;
        uint32_t seed = 1;
        Filter hp, lp;
        bool captured = false;
        double read(double delay) const SUPPLE_RT;
        double frozenRead() const SUPPLE_RT;
        double random() SUPPLE_RT;
    };
    template <class T>
    void render(const T* const*, T* const*, uint32_t) SUPPLE_RT;
    double decode(size_t, double) const SUPPLE_RT;
    double encode(size_t, double) const SUPPLE_RT;
    Values values_ = defaults();
    std::array<Ramp, kNumParams> ramps_{};
    std::array<Ramp, 3> modes_{};
    std::array<Line, 2> lines_;
    Telemetry telemetry_;
    double fs_ = 48000, tempo_ = 120, phase_ = 0, glideAlpha_ = 0, env_ = -120, attack_ = 0,
           release_ = 0;
    uint64_t sample_ = 0;
    int channels_ = 2;
    bool started_ = false, freezeTarget_ = false;
};
}  // namespace supple::delay
