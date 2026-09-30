#pragma once
#include <cstdint>
#include <memory>
#include "eq/params.h"
#include "supple/base/rt.h"
namespace supple::eq {
// The affected channel's response; channel selection is applied around each band.
double magnitudeDb(const Values&, double hz, double fs);
double bandMagnitudeDb(const Values&, std::size_t band, double hz, double fs);
double compensationDb(const Values&, double fs);
class Processor {
  public:
    Processor();
    ~Processor();
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;
    void prepare(double sampleRate, std::uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(std::size_t index, double physical) SUPPLE_RT;
    // Control thread: compile/publish FIR kernels, reclaim retired programs.
    void setLinearValues(const Values&);
    void collect();
    int latency() const;
    void process(const float* const* in, float* const* out, std::uint32_t frames) SUPPLE_RT;
    void process(const double* const* in, double* const* out, std::uint32_t frames) SUPPLE_RT;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace supple::eq
