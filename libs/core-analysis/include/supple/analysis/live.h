#pragma once

#include "supple/analysis/analysis.h"

namespace supple::analysis {

struct LiveMeasurements {
    double seconds = 0;
    double momentary = -120, shortTerm = -120, integrated = -120, lra = 0;
    double truePeak = -120, samplePeak = -120, maxMomentary = -120, maxShortTerm = -120;
    double correlation = 1, width = 0, balance = 0, centroid = 0;
    std::array<double, kThirdOctaves> spectrum{};
};

// Consumer-thread streaming measurements. Fixed 100 ms observation grid,
// rolling complete loudness blocks, stereo statistics on the same observation grid, Hann FFT
// spectra averaged in power with a 300 ms time constant. No audio-thread use.
class LiveMeter {
  public:
    void prepare(double sampleRate, int channels, double windowSeconds);
    void reset();
    void process(std::span<const float* const> input, int frames);
    LiveMeasurements measurements() const;
    double elapsedSeconds() const { return static_cast<double>(samples_) / fs_; }

  private:
    struct StereoSums {
        double l = 0, r = 0, ll = 0, rr = 0, lr = 0;
    };
    double fs_ = 48000, windowSeconds_ = 3;
    int channels_ = 2, hop_ = 4800;
    std::uint64_t samples_ = 0, stereoSamples_ = 0;
    LoudnessMeter loudness_;
    std::vector<StereoSums> stereoHistory_;
    StereoSums stereo_{};
    dsp::Fft fft_;
    std::vector<double> audio_, window_, fftInput_, smoothedPower_;
    std::vector<std::complex<double>> fftOutput_;
    double norm_ = 1, smoothing_ = 0;
    LiveMeasurements latest_{};
    void spectrum();
};

// A compact analysis-report containing only the descriptors measured live.
json::Value toJson(const LiveMeasurements& m, double sampleRate, int channels);

}  // namespace supple::analysis
