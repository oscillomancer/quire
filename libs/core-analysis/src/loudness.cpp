// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/analysis/loudness.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace supple::analysis {
namespace {
constexpr double negInf = -std::numeric_limits<double>::infinity();
constexpr double offset = -0.691;
int channels(Layout layout) {
    switch (layout) {
        case Layout::Mono:
            return 1;
        case Layout::Stereo:
            return 2;
        case Layout::Surround50:
            return 5;
        case Layout::Surround51:
            return 6;
    }
    return 0;
}
}  // namespace

double LoudnessMeter::loudness(double energy) {
    return energy > 0 ? offset + 10 * std::log10(energy) : negInf;
}
int LoudnessMeter::bin(double lu) {
    return std::clamp(static_cast<int>(std::floor((lu + 120) * 100 + 0.5)), 0, kBins - 1);
}
void LoudnessMeter::prepare(double sampleRate, Layout layout, Mode mode, double maxSeconds) {
    channels_ = channels(layout);
    mode_ = mode;
    hop_ = static_cast<int>(std::llround(sampleRate * 0.1));
    window400_ = hop_ * 4;
    window3_ = hop_ * 30;
    factor_ = sampleRate <= 48000 ? 4 : (sampleRate <= 96000 ? 2 : 1);
    ring_.resize(static_cast<std::size_t>(window3_));
    const auto capacity =
        static_cast<std::size_t>(std::ceil((maxSeconds > 0 ? maxSeconds : 600) * 10)) + 40;
    if (mode_ == Mode::Exact) {
        exactBlocks_.resize(capacity);
        exactShort_.resize(capacity);
    } else {
        exactBlocks_.clear();
        exactShort_.clear();
    }
    weights_ = {1, 1, 1, 1.41, 1.41, 0};
    if (layout == Layout::Surround51) weights_ = {1, 1, 1, 0, 1.41, 1.41};
    if (layout == Layout::Mono || layout == Layout::Stereo) weights_ = {1, 1, 0, 0, 0, 0};
    for (int c = 0; c < channels_; ++c) {
        shelf_[static_cast<std::size_t>(c)].set(dsp::kShelf(sampleRate));
        highpass_[static_cast<std::size_t>(c)].set(dsp::kHighpass(sampleRate));
        oversamplers_[static_cast<std::size_t>(c)].prepare(factor_);
    }
    reset();
}
void LoudnessMeter::reset() SUPPLE_RT {
    for (auto& f : shelf_) f.reset();
    for (auto& f : highpass_) f.reset();
    for (auto& f : oversamplers_) f.reset();
    std::fill(ring_.begin(), ring_.end(), 0);
    blocksHist_.fill(0);
    shortHist_.fill(0);
    blockEnergyHist_.fill(0);
    shortEnergyHist_.fill(0);
    blockCount_ = shortCount_ = 0;
    samples_ = 0;
    sum400_ = sum3_ = peak_ = samplePeak_ = 0;
    momentary_ = shortTerm_ = negInf;
    maxMomentaryEnergy_ = maxShortEnergy_ = 0;
}
void LoudnessMeter::process(std::span<const float* const> input, int frames) SUPPLE_RT {
    if (ring_.empty() || input.size() < static_cast<std::size_t>(channels_) || frames <= 0) return;
    dsp::DenormalGuard guard;
    std::array<double, 4> phases{};
    for (int i = 0; i < frames; ++i) {
        double energy = 0;
        for (int c = 0; c < channels_; ++c) {
            const float x = input[static_cast<std::size_t>(c)][i];
            samplePeak_ = std::max(samplePeak_, std::abs(static_cast<double>(x)));
            peak_ = std::max(peak_, std::abs(static_cast<double>(x)));
            if (factor_ > 1) {
                oversamplers_[static_cast<std::size_t>(c)].process(
                    x, std::span<double>(phases).first(static_cast<std::size_t>(factor_)));
                for (int p = 0; p < factor_; ++p)
                    peak_ = std::max(peak_, std::abs(phases[static_cast<std::size_t>(p)]));
            }
            if (weights_[static_cast<std::size_t>(c)] != 0) {
                const double y = highpass_[static_cast<std::size_t>(c)].process(
                    shelf_[static_cast<std::size_t>(c)].process(static_cast<double>(x)));
                energy += weights_[static_cast<std::size_t>(c)] * y * y;
            }
        }
        const std::size_t pos = static_cast<std::size_t>(samples_ % ring_.size());
        sum400_ += energy;
        if (samples_ >= static_cast<std::uint64_t>(window400_)) {
            const auto old = (samples_ - static_cast<std::uint64_t>(window400_)) % ring_.size();
            sum400_ -= ring_[static_cast<std::size_t>(old)];
        }
        sum3_ += energy;
        if (samples_ >= static_cast<std::uint64_t>(window3_)) sum3_ -= ring_[pos];
        ring_[pos] = energy;
        ++samples_;
        if (samples_ >= static_cast<std::uint64_t>(window400_))
            maxMomentaryEnergy_ = std::max(maxMomentaryEnergy_, sum400_ / window400_);
        if (samples_ >= static_cast<std::uint64_t>(window3_))
            maxShortEnergy_ = std::max(maxShortEnergy_, sum3_ / window3_);
        if (samples_ % static_cast<std::uint64_t>(hop_) != 0) continue;
        if (samples_ >= static_cast<std::uint64_t>(window400_)) {
            const double e = std::max(0.0, sum400_) / window400_;
            momentary_ = loudness(e);
            if (mode_ == Mode::Exact) {
                if (blockCount_ < exactBlocks_.size()) exactBlocks_[blockCount_++] = e;
            } else {
                if (momentary_ >= -70) {
                    const auto b = static_cast<std::size_t>(bin(momentary_));
                    ++blocksHist_[b];
                    blockEnergyHist_[b] += e;
                }
                ++blockCount_;
            }
        }
        if (samples_ >= static_cast<std::uint64_t>(window3_)) {
            const double e = std::max(0.0, sum3_) / window3_;
            shortTerm_ = loudness(e);
            if (mode_ == Mode::Exact) {
                if (shortCount_ < exactShort_.size()) exactShort_[shortCount_++] = e;
            } else {
                if (shortTerm_ >= -70) {
                    const auto b = static_cast<std::size_t>(bin(shortTerm_));
                    ++shortHist_[b];
                    shortEnergyHist_[b] += e;
                }
                ++shortCount_;
            }
        }
    }
}
double LoudnessMeter::integrated() const {
    double sum = 0;
    std::size_t count = 0;
    if (mode_ == Mode::Exact) {
        for (std::size_t i = 0; i < blockCount_; ++i)
            if (loudness(exactBlocks_[i]) >= -70) {
                sum += exactBlocks_[i];
                ++count;
            }
    } else {
        for (int b = 0; b < kBins; ++b) {
            sum += blockEnergyHist_[static_cast<std::size_t>(b)];
            count += blocksHist_[static_cast<std::size_t>(b)];
        }
    }
    if (!count) return negInf;
    const double gate = loudness(sum / count) - 10;
    sum = 0;
    count = 0;
    if (mode_ == Mode::Exact) {
        for (std::size_t i = 0; i < blockCount_; ++i)
            if (loudness(exactBlocks_[i]) >= -70 && loudness(exactBlocks_[i]) >= gate) {
                sum += exactBlocks_[i];
                ++count;
            }
    } else {
        for (int b = 0; b < kBins; ++b)
            if (blocksHist_[static_cast<std::size_t>(b)] &&
                loudness(blockEnergyHist_[static_cast<std::size_t>(b)] /
                         blocksHist_[static_cast<std::size_t>(b)]) >= gate) {
                sum += blockEnergyHist_[static_cast<std::size_t>(b)];
                count += blocksHist_[static_cast<std::size_t>(b)];
            }
    }
    return count ? loudness(sum / count) : negInf;
}
double LoudnessMeter::loudnessRange() const {
    double sum = 0;
    std::size_t count = 0;
    if (mode_ == Mode::Exact) {
        for (std::size_t i = 0; i < shortCount_; ++i)
            if (loudness(exactShort_[i]) >= -70) {
                sum += exactShort_[i];
                ++count;
            }
    } else {
        for (int b = 0; b < kBins; ++b) {
            sum += shortEnergyHist_[static_cast<std::size_t>(b)];
            count += shortHist_[static_cast<std::size_t>(b)];
        }
    }
    if (!count) return negInf;
    const double gate = loudness(sum / count) - 20;
    if (mode_ == Mode::Exact) {
        std::vector<double> values;
        values.reserve(shortCount_);
        for (std::size_t i = 0; i < shortCount_; ++i) {
            const double lu = loudness(exactShort_[i]);
            if (lu >= -70 && lu >= gate) values.push_back(lu);
        }
        if (values.empty()) return negInf;
        std::sort(values.begin(), values.end());
        auto percentile = [&](double p) {
            return values[static_cast<std::size_t>(std::llround((values.size() - 1) * p))];
        };
        return percentile(0.95) - percentile(0.10);
    }
    std::size_t total = 0;
    for (int b = 0; b < kBins; ++b)
        if (-120.0 + b * 0.01 >= gate - 0.005) total += shortHist_[static_cast<std::size_t>(b)];
    if (!total) return negInf;
    auto percentile = [&](double p) {
        const auto target = static_cast<std::size_t>(std::llround((total - 1) * p));
        std::size_t cumulative = 0;
        for (int b = 0; b < kBins; ++b) {
            if (-120.0 + b * 0.01 < gate - 0.005) continue;
            cumulative += shortHist_[static_cast<std::size_t>(b)];
            if (cumulative > target) return -120.0 + b * 0.01;
        }
        return 30.0;
    };
    return percentile(0.95) - percentile(0.10);
}
double LoudnessMeter::maxMomentary() const { return loudness(maxMomentaryEnergy_); }
double LoudnessMeter::maxShortTerm() const { return loudness(maxShortEnergy_); }
double LoudnessMeter::truePeak() const { return peak_ > 0 ? 20 * std::log10(peak_) : negInf; }
double LoudnessMeter::samplePeak() const {
    return samplePeak_ > 0 ? 20 * std::log10(samplePeak_) : negInf;
}
}  // namespace supple::analysis
