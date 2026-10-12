// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Measurements an editor makes from the audio feed (core/AudioFeed.h): a
// spectrum and an RMS level. Main thread, in idle or animate; each is
// driven by real elapsed seconds, so its motion is the same at any frame
// rate.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/Fft.h"
#include "gui/AudioHistory.h"

namespace supple::gui {

// Magnitude per frequency of the newest stretch of a signal, Hann-windowed,
// in dBFS: a full-scale sine reads 0 dB at its frequency. Rises at once,
// falls at fallDbPerSecond.
class Spectrum {
  public:
    static constexpr float kFloorDb = -120.0f;
    float fallDbPerSecond = 36.0f;

    // `size` samples per analysis, a power of two: 4096 resolves 12 Hz at
    // 48 kHz, enough for a bass note.
    explicit Spectrum(size_t size = 4096) : fft_(size) {
        const size_t n = fft_.size();
        window_.resize(n);
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            window_[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(n));
            sum += window_[i];
        }
        gain_ = 2.0 / sum;  // one-sided, window-compensated: a sine's amplitude
        buffer_.resize(n);
        samples_.resize(n);
        db_.assign(n / 2 + 1, kFloorDb);
    }

    size_t size() const { return fft_.size(); }
    double sampleRate() const { return sampleRate_; }

    // Analyse the newest size() samples; `seconds` since the last call.
    void analyse(const AudioHistory& audio, Signal signal, double sampleRate, double seconds) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        const size_t n = fft_.size();
        audio.latest(signal, samples_.data(), n);
        for (size_t i = 0; i < n; ++i) buffer_[i] = {samples_[i] * window_[i], 0.0};
        fft_.forward(buffer_.data());
        const float fall = fallDbPerSecond * static_cast<float>(std::clamp(seconds, 0.0, 0.25));
        moving_ = false;
        for (size_t k = 0; k < db_.size(); ++k) {
            const double magnitude = std::abs(buffer_[k]) * gain_ * (k == 0 || k == n / 2 ? 0.5 : 1.0);
            const float now = std::max(kFloorDb, static_cast<float>(20.0 * std::log10(magnitude + 1e-30)));
            db_[k] = std::max(now, std::max(kFloorDb, db_[k] - fall));
            moving_ = moving_ || db_[k] > kFloorDb;
        }
    }
    // Decay without analysing stale samples when the host stops processing.
    void fall(double seconds) {
        const float amount = fallDbPerSecond * static_cast<float>(std::max(0.0, seconds));
        moving_ = false;
        for (float& db : db_) {
            db = std::max(kFloorDb, db - amount);
            moving_ = moving_ || db > kFloorDb;
        }
    }

    void clear() {
        std::fill(db_.begin(), db_.end(), kFloorDb);
        moving_ = false;
    }

    // dB at a frequency, between bins.
    float db(double hz) const {
        const double bin = std::clamp(hz * static_cast<double>(fft_.size()) / sampleRate_, 0.0, static_cast<double>(db_.size() - 1));
        const size_t k = static_cast<size_t>(bin);
        const float t = static_cast<float>(bin - static_cast<double>(k));
        return k + 1 < db_.size() ? db_[k] + (db_[k + 1] - db_[k]) * t : db_[k];
    }
    // The loudest bin from lo to hi Hz: what a column of a drawn spectrum
    // covers, so a narrow peak is not lost between columns.
    float peak(double lo, double hi) const {
        const double scale = static_cast<double>(fft_.size()) / sampleRate_;
        const auto first = static_cast<size_t>(std::clamp(std::ceil(lo * scale), 0.0, static_cast<double>(db_.size() - 1)));
        const auto last = static_cast<size_t>(std::clamp(std::floor(hi * scale), 0.0, static_cast<double>(db_.size() - 1)));
        if (last < first) return db(0.5 * (lo + hi));
        float most = kFloorDb;
        for (size_t k = first; k <= last; ++k) most = std::max(most, db_[k]);
        return most;
    }
    // Something is above the floor: still worth drawing.
    bool moving() const { return moving_; }

  private:
    dsp::Fft fft_;
    std::vector<double> window_;
    std::vector<std::complex<double>> buffer_;
    std::vector<float> samples_;
    std::vector<float> db_;
    double gain_ = 1.0;
    double sampleRate_ = 48000.0;
    bool moving_ = false;
};

// A signal's RMS over a sliding 300 ms (an exponential window), in dBFS: a
// full-scale sine reads -3 dB. Push every new sample; EditorBase's own
// meters fall by themselves once the host stops sending any.
class RmsMeter {
  public:
    static constexpr float kFloorDb = -72.0f;
    static constexpr double kWindowSeconds = 0.3;

    void push(const float* samples, size_t n, double sampleRate) {
        const double a = std::exp(-1.0 / (kWindowSeconds * std::max(1.0, sampleRate)));
        for (size_t i = 0; i < n; ++i) meanSquare_ = a * meanSquare_ + (1.0 - a) * samples[i] * samples[i];
    }
    float levelDb() const {
        return std::max(kFloorDb, static_cast<float>(10.0 * std::log10(meanSquare_ + 1e-30)));
    }
    // No samples for `seconds` (the host stopped): fall as that much silence
    // would.
    void fall(double seconds) { meanSquare_ *= std::exp(-std::max(0.0, seconds) / kWindowSeconds); }
    bool moving() const { return levelDb() > kFloorDb; }
    void reset() { meanSquare_ = 0.0; }

  private:
    double meanSquare_ = 0.0;
};

}  // namespace supple::gui
