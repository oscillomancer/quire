// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/analysis/live.h"

#include <algorithm>
#include <cmath>

namespace supple::analysis {
namespace {
double level(double x, double fallback = -120) { return std::isfinite(x) ? x : fallback; }
}  // namespace

void LiveMeter::prepare(double sampleRate, int channels, double windowSeconds) {
    fs_ = sampleRate;
    channels_ = channels;
    windowSeconds_ = windowSeconds;
    hop_ = static_cast<int>(std::llround(fs_ * 0.1));
    loudness_.prepare(fs_, channels == 1 ? Layout::Mono : Layout::Stereo,
                      LoudnessMeter::Mode::Histogram, 0, windowSeconds);
    // Stereo sums expire on the same 100 ms grid as loudness.
    stereoHistory_.resize(
        windowSeconds > 0 ? static_cast<std::size_t>(std::llround(10 * windowSeconds)) : 0);
    const auto n = std::size_t{1} << static_cast<int>(std::round(std::log2(fs_ * 0.0427)));
    fft_.prepare(n);
    audio_.resize(n);
    window_.resize(n);
    fftInput_.resize(n);
    fftOutput_.resize(n / 2 + 1);
    smoothedPower_.resize(n / 2 + 1);
    double w2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        window_[i] = dsp::hann(i, n);
        w2 += window_[i] * window_[i];
    }
    norm_ = 2 / (static_cast<double>(n) * w2);
    smoothing_ = std::exp(-static_cast<double>(hop_) / (0.3 * fs_));
    reset();
}

void LiveMeter::reset() {
    loudness_.reset();
    stereo_ = {};
    samples_ = stereoSamples_ = 0;
    std::fill(stereoHistory_.begin(), stereoHistory_.end(), StereoSums{});
    std::fill(audio_.begin(), audio_.end(), 0);
    std::fill(smoothedPower_.begin(), smoothedPower_.end(), 0);
    latest_ = {};
    latest_.spectrum.fill(-120);
}

void LiveMeter::process(std::span<const float* const> input, int frames) {
    if (audio_.empty() || frames <= 0 || input.size() < static_cast<std::size_t>(channels_)) return;
    dsp::DenormalGuard guard;
    for (int i = 0; i < frames; ++i) {
        const double l = static_cast<double>(input[0][i]);
        const double r = channels_ == 1 ? l : static_cast<double>(input[1][i]);
        StereoSums next{l, r, l * l, r * r, l * r}, old{};
        if (!stereoHistory_.empty()) {
            const auto pos = static_cast<std::size_t>(
                (samples_ / static_cast<std::uint64_t>(hop_)) % stereoHistory_.size());
            auto& bucket = stereoHistory_[pos];
            if (samples_ % static_cast<std::uint64_t>(hop_) == 0) {
                old = bucket;
                bucket = {};
                if (samples_ >= static_cast<std::uint64_t>(hop_) * stereoHistory_.size())
                    stereoSamples_ -= static_cast<std::uint64_t>(hop_);
            }
            bucket.l += next.l;
            bucket.r += next.r;
            bucket.ll += next.ll;
            bucket.rr += next.rr;
            bucket.lr += next.lr;
        }
        stereo_.l += next.l - old.l;
        stereo_.r += next.r - old.r;
        stereo_.ll += next.ll - old.ll;
        stereo_.rr += next.rr - old.rr;
        stereo_.lr += next.lr - old.lr;
        ++stereoSamples_;
        audio_[static_cast<std::size_t>(samples_ % audio_.size())] = 0.5 * (l + r);
        ++samples_;
        // Feed the existing meter on a sample grid, independent of caller blocks.
        const float* ptr[]{input[0] + i, channels_ == 1 ? input[0] + i : input[1] + i};
        loudness_.process(std::span<const float* const>(ptr, static_cast<std::size_t>(channels_)),
                          1);
        if (samples_ % static_cast<std::uint64_t>(hop_) == 0) spectrum();
    }
}

void LiveMeter::spectrum() {
    const auto n = audio_.size();
    for (std::size_t i = 0; i < n; ++i)
        fftInput_[i] = audio_[static_cast<std::size_t>((samples_ + i) % n)] * window_[i];
    fft_.forwardRt(fftInput_, fftOutput_);
    double power = 0, weighted = 0;
    const double df = fs_ / static_cast<double>(n);
    for (std::size_t k = 0; k < fftOutput_.size(); ++k) {
        const double p = std::norm(fftOutput_[k]) * norm_ * (k == 0 || k == n / 2 ? 0.5 : 1.0);
        smoothedPower_[k] = p + smoothing_ * (smoothedPower_[k] - p);
        if (smoothedPower_[k] < 1e-30) smoothedPower_[k] = 0;
        const double hz = static_cast<double>(k) * df;
        if (hz >= 20 && hz <= 20000) {
            power += smoothedPower_[k];
            weighted += hz * smoothedPower_[k];
        }
    }
    latest_.centroid = power > 0 ? weighted / power : 0;
    for (int b = 0; b < kThirdOctaves; ++b) {
        const double fc = thirdOctaveCentre(b), lo = fc / std::pow(2.0, 1.0 / 6),
                     hi = std::min(fs_ / 2, fc * std::pow(2.0, 1.0 / 6));
        double sum = 0;
        for (std::size_t k = 1; k < smoothedPower_.size(); ++k) {
            const double a = (static_cast<double>(k) - 0.5) * df, z = a + df;
            sum += smoothedPower_[k] * std::max(0.0, std::min(z, hi) - std::max(a, lo)) / df;
        }
        latest_.spectrum[static_cast<std::size_t>(b)] = 10 * std::log10(std::max(1e-12, sum / 0.5));
    }
}

LiveMeasurements LiveMeter::measurements() const {
    auto m = latest_;
    m.seconds = static_cast<double>(samples_) / fs_;
    if (windowSeconds_ > 0) m.seconds = std::min(m.seconds, windowSeconds_);
    m.momentary = level(loudness_.momentary());
    m.shortTerm = level(loudness_.shortTerm());
    m.integrated = level(loudness_.integrated());
    m.lra = level(loudness_.loudnessRange(), 0);
    m.truePeak = level(loudness_.truePeak());
    m.samplePeak = level(loudness_.samplePeak());
    m.maxMomentary = level(loudness_.maxMomentary());
    m.maxShortTerm = level(loudness_.maxShortTerm());
    const double count = static_cast<double>(std::max<std::uint64_t>(1, stereoSamples_));
    const double ll = std::max(0.0, stereo_.ll - stereo_.l * stereo_.l / count);
    const double rr = std::max(0.0, stereo_.rr - stereo_.r * stereo_.r / count);
    m.correlation =
        ll > 0 && rr > 0
            ? std::clamp((stereo_.lr - stereo_.l * stereo_.r / count) / std::sqrt(ll * rr), -1.0,
                         1.0)
            : (ll == rr ? 1 : 0);
    const double total = std::max(0.0, stereo_.ll + stereo_.rr);
    m.width = total > 0 ? std::clamp((total - 2 * stereo_.lr) / (2 * total), 0.0, 1.0) : 0;
    m.balance =
        total > 0
            ? std::clamp(10 * std::log10(std::max(1e-30, stereo_.rr) / std::max(1e-30, stereo_.ll)),
                         -120.0, 120.0)
            : 0;
    return m;
}

json::Value toJson(const LiveMeasurements& m, double sampleRate, int channels) {
    auto out = json::Value::object();
    out["schema"] = "supple.analysis-report/1.0";
    auto source = json::Value::object();
    source["seconds"] = m.seconds;
    source["sample_rate"] = sampleRate;
    source["channels"] = channels;
    source["layout"] = channels == 1 ? "mono" : "stereo";
    auto lu = json::Value::object();
    lu["integrated_lufs"] = m.integrated;
    lu["lra_lu"] = m.lra;
    lu["max_momentary_lufs"] = m.maxMomentary;
    lu["max_short_term_lufs"] = m.maxShortTerm;
    lu["true_peak_dbtp"] = m.truePeak;
    lu["sample_peak_dbfs"] = m.samplePeak;
    lu["plr_lu"] = m.truePeak - m.integrated;
    lu["momentary_lufs"] = json::Value::array();
    lu["momentary_lufs"].push_back(m.momentary);
    lu["short_term_lufs"] = json::Value::array();
    lu["short_term_lufs"].push_back(m.shortTerm);
    auto sp = json::Value::object();
    sp["centroid_hz"]["mean"] = m.centroid;
    sp["centroid_hz"]["median"] = m.centroid;
    sp["ltas_db"] = json::Value::array();
    for (double value : m.spectrum) sp["ltas_db"].push_back(value);
    auto stereo = json::Value::object();
    stereo["correlation"] = m.correlation;
    stereo["width"] = m.width;
    stereo["balance_db"] = m.balance;
    const auto round = [](json::Value& value, double scale) {
        value = std::round(value.asDouble() * scale) / scale;
    };
    round(source["seconds"], 1000);
    for (auto& member : lu.asObject()) {
        if (member.value.isArray())
            for (auto& x : member.value.asArray()) round(x, 10);
        else
            round(member.value, 10);
    }
    round(stereo["correlation"], 1000);
    round(stereo["width"], 1000);
    round(stereo["balance_db"], 10);
    for (auto& x : sp["ltas_db"].asArray()) round(x, 10);
    const double hzScale =
        m.centroid >= 100 ? std::pow(10.0, 2 - std::floor(std::log10(m.centroid))) : 1;
    round(sp["centroid_hz"]["mean"], hzScale);
    round(sp["centroid_hz"]["median"], hzScale);
    out["source"] = std::move(source);
    out["loudness"] = std::move(lu);
    out["spectral"] = std::move(sp);
    out["stereo"] = std::move(stereo);
    return out;
}
}  // namespace supple::analysis
