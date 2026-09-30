#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "comp/params.h"
#include "supple/dsp/dsp.h"

namespace supple::comp {
class Processor {
  public:
    struct Telemetry {
        double inputPeakDb = -120, outputPeakDb = -120, gr = 0, detectorDb = -120;
    };
    Processor();
    Status prepare(double sampleRate, std::uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(std::size_t index, double physical) SUPPLE_RT;
    std::uint32_t latency() const SUPPLE_RT { return latencyFor(target_, sampleRate_); }
    double reduction() const SUPPLE_RT { return std::min(reduction_[0], reduction_[1]); }
    Telemetry takeTelemetry() SUPPLE_RT;
    template <class T>
    void process(const T* const* input, T* const* output, std::uint32_t frames,
                 const T* const* key = nullptr) SUPPLE_RT {
        dsp::DenormalGuard guard;
        if (!output || delay_[0].empty()) return;
        started_ = true;
        for (std::uint32_t i = 0; i < frames; ++i) {
            std::array<double, 2> x{}, k{}, y{};
            for (int c = 0; c < channels_; ++c) {
                const auto n = static_cast<std::size_t>(c);
                x[n] = input && input[c] ? static_cast<double>(input[c][i]) : 0;

                if (!std::isfinite(x[n])) x[n] = 0;
            }
            // The optional key always has two channel pointers, also in mono.
            if (key)
                for (std::size_t c = 0; c < 2; ++c) {
                    k[c] = key[c] ? static_cast<double>(key[c][i]) : 0;
                    if (!std::isfinite(k[c])) k[c] = 0;
                }
            if (channels_ == 1) x[1] = x[0];
            tick(x, k, y);
            for (int c = 0; c < channels_; ++c)
                if (output[c]) output[c][i] = static_cast<T>(y[static_cast<std::size_t>(c)]);
        }
    }

  private:
    struct Filter {
        double a = 0, b = 0;
        double run(double x, double g, bool highpass) SUPPLE_RT;
    };
    struct Minimum {
        struct Entry {
            std::uint64_t time = 0;
            double value = 1;
        };
        std::vector<Entry> queue;
        std::size_t read = 0, write = 0;
        double push(std::uint64_t time, double value, std::uint32_t width) SUPPLE_RT;
        void reset() SUPPLE_RT { read = write = 0; }
    };
    void tick(const std::array<double, 2>&, const std::array<double, 2>&,
              std::array<double, 2>&) SUPPLE_RT;
    Values target_, current_, derived_{};
    double hpG_ = 0, lpG_ = 0, rmsAlpha_ = 0, attackAlpha_ = 0, releaseAlpha_ = 0, slowAlpha_ = 0;
    double outGain_ = 1, wetGain_ = 1, ceilingGain_ = 1, inverseRatio_ = .25;
    std::array<double, 3> peaks_{};
    double sampleRate_ = 48000, smooth_ = 0, switch_ = 0;
    int channels_ = 2;
    bool started_ = false;
    std::array<std::vector<double>, 2> delay_, average_;
    std::array<Minimum, 2> minimum_;
    std::array<dsp::PolyphaseUpsampler, 2> interpolators_;
    std::array<std::array<Filter, 2>, 3> hp_{}, lp_{};
    std::array<std::array<double, 2>, 3> rms_{};
    std::array<double, 2> reduction_{}, feedback_{}, held_{}, limiterGain_{1, 1}, averageSum_{};
    std::size_t head_ = 0, averageHead_ = 0, averageSize_ = 0;
    std::uint64_t clock_ = 0;
    Telemetry meters_;
};
}  // namespace supple::comp
