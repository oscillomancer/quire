// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cmath>
#include <vector>
#include "reverb/params.h"
#include "supple/dsp/dsp.h"
namespace supple::reverb {
class Processor {
  public:
    Processor();
    Status prepare(double sampleRate, std::uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(std::size_t, double physical) SUPPLE_RT;
    std::uint32_t latency() const SUPPLE_RT { return 0; }
    struct Telemetry {
        double inputPeakDb = -120, outputPeakDb = -120;
    };
    void setTelemetryEnabled(bool enabled) SUPPLE_RT {
        telemetryEnabled_ = enabled;
        if (!enabled) peakIn_ = peakOut_ = 0;
    }
    Telemetry takeTelemetry() SUPPLE_RT;
    template <class T>
    void process(const T* const* in, T* const* out, std::uint32_t frames) SUPPLE_RT {
        dsp::DenormalGuard guard;
        if (!out || lines_[0].data.empty()) return;
        if (!started_) {
            freeze_ = current_[kFreeze] > .5 ? 1 : 0;
            held_ = false;
            update();
            held_ = freeze_ == 1;
            started_ = true;
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            std::array<double, 2> x{}, y{};
            for (int c = 0; c < channels_; ++c) {
                auto j = static_cast<std::size_t>(c);
                x[j] = in && in[c] ? static_cast<double>(in[c][i]) : 0;
                if (!std::isfinite(x[j])) x[j] = 0;
            }
            if (channels_ == 1) x[1] = x[0];
            tick(x, y);
            for (int c = 0; c < channels_; ++c)
                if (out[c]) out[c][i] = static_cast<T>(y[static_cast<std::size_t>(c)]);
        }
    }

  private:
    struct Delay {
        std::vector<double> data;
        std::size_t head = 0, mask = 0;
        void prepare(double samples);
        void reset() SUPPLE_RT;
        double read(double samples) const SUPPLE_RT;
        double modulated(double samples,
                         const std::array<std::array<double, 12>, 257>& kernel) const SUPPLE_RT;
        double integer(std::size_t samples) const SUPPLE_RT {
            return data[(head - samples) & mask];
        }
        void write(double x) SUPPLE_RT {
            data[head] = std::abs(x) < 1e-30 ? 0 : x;
            head = (head + 1) & mask;
        }
    };
    struct Shelf {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
        bool flat = true;
        double run(double x) SUPPLE_RT;
        void design(double frequency, double ratio, double fs, bool high) SUPPLE_RT;
    };
    struct Cut {
        double a1 = 1, a2 = 0, a3 = 0, s1 = 0, s2 = 0;
        void design(double frequency, double fs) SUPPLE_RT;
        double run(double x, bool high) SUPPLE_RT;
    };
    struct Glide {
        double value = 0, stage = 0;
        void tick(double target, double alpha) SUPPLE_RT;
    };
    void update() SUPPLE_RT;
    void tick(const std::array<double, 2>&, std::array<double, 2>&) SUPPLE_RT;
    Values target_, current_, step_{};
    std::array<std::uint32_t, kNumParams> remaining_{};
    std::uint32_t moving_ = 0;
    std::array<Delay, 16> lines_;
    std::array<std::array<double, 12>, 257> interpolation_{};
    std::array<std::array<Delay, 4>, 2> diffusers_;
    std::array<Delay, 2> early_, predelay_;
    std::array<std::array<Cut, 2>, 2> cuts_{};
    std::array<Shelf, 16> low_{}, high_{};
    std::array<Glide, 16> lengths_{};
    Glide pre_;
    std::array<double, 16> baseMs_{}, lengthTarget_{}, gain_{}, oscSin_{}, oscCos_{}, oscStepSin_{},
        oscStepCos_{};
    std::array<std::size_t, 16> heldLength_{};
    std::array<std::array<double, 12>, 2> tapMs_{}, tapGain_{}, tapLength_{};
    std::array<std::array<double, 4>, 2> allpassMs_{}, allpassLength_{};
    std::array<std::array<Glide, 12>, 2> tapRead_{};
    std::array<std::array<Glide, 4>, 2> allpassRead_{};
    double fs_ = 48000, glideAlpha_ = 0, preAlpha_ = 0, scale_ = 1, freeze_ = 0, freezeStep_ = 0,
           earlyGain_ = 0, outputGain_ = 1, outputTarget_ = 1, outputStep_ = 0;
    double allpassGain_ = 0, modDepth_ = 0;
    int channels_ = 2;
    bool started_ = false, dirty_ = true, coefficientsDirty_ = true, held_ = false,
         telemetryEnabled_ = false;
    std::uint64_t clock_ = 0;
    double peakIn_ = 0, peakOut_ = 0;
};
}  // namespace supple::reverb
