// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "playrate_dsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "signalsmith-stretch/signalsmith-stretch.h"

namespace supple::engine::detail {

namespace {
double boundedRate(double rate) SUPPLE_RT {
    return std::isfinite(rate) ? std::clamp(rate, .25, 4.) : 1.;
}
constexpr long kSeed = 20261016;
}  // namespace

void PlayrateClock::reset(double timeline, double rate) SUPPLE_RT {
    timeline_ = timeline;
    scalar_ = from_ = target_ = boundedRate(rate);
    rampFrames_ = rampAt_ = 0;
}
void PlayrateClock::setRate(double rate, std::uint32_t frames) SUPPLE_RT {
    rate = boundedRate(rate);
    if (target_ == rate) return;
    from_ = scalar_;
    target_ = rate;
    rampFrames_ = frames;
    rampAt_ = 0;
    if (frames == 0) scalar_ = target_;
}
void PlayrateClock::setEnvelope(Envelope envelope, const void* context) SUPPLE_RT {
    envelope_ = envelope;
    context_ = context;
}
double PlayrateClock::rate() const SUPPLE_RT {
    return envelope_ ? boundedRate(envelope_(context_, timeline_)) : scalar_;
}
void PlayrateClock::advance(std::span<double> boundaries) SUPPLE_RT {
    if (boundaries.empty()) return;
    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        boundaries[i] = timeline_;
        timeline_ += rate();
        if (rampAt_ < rampFrames_) {
            ++rampAt_;
            scalar_ = rampAt_ == rampFrames_
                          ? target_
                          : from_ + (target_ - from_) * (double(rampAt_) / rampFrames_);
        }
    }
    boundaries.back() = timeline_;
}
std::size_t PlayrateClock::crossing(std::span<const double> boundaries, double timeline) SUPPLE_RT {
    return static_cast<std::size_t>(
        std::lower_bound(boundaries.begin(), boundaries.end(), timeline) - boundaries.begin());
}

TapeTable::TapeTable() : coefficients_(kTaps * kPhases * kBanks) {
    for (int bank = 0; bank < kBanks; ++bank) {
        // Guard band for the finite window, also at rate <= 1. Unity's exact
        // integer path below bypasses filtering entirely.
        const double cutoff = .94 / (1. + bank * .25);
        for (int phase = 0; phase < kPhases; ++phase) {
            const double frac = double(phase) / kPhases;
            double sum = 0;
            const auto base = static_cast<std::size_t>((bank * kPhases + phase) * kTaps);
            for (int tap = 0; tap < kTaps; ++tap) {
                const double x = double(tap - (kTaps / 2 - 1)) - frac;
                const double a = std::numbers::pi * x * cutoff;
                const double sinc = std::abs(a) < 1e-12 ? 1. : std::sin(a) / a;
                const double w = .42 + .5 * std::cos(2 * std::numbers::pi * x / kTaps) +
                                 .08 * std::cos(4 * std::numbers::pi * x / kTaps);
                coefficients_[base + static_cast<std::size_t>(tap)] =
                    static_cast<float>(cutoff * sinc * w);
                sum += cutoff * sinc * w;
            }
            for (int tap = 0; tap < kTaps; ++tap)
                coefficients_[base + static_cast<std::size_t>(tap)] /= static_cast<float>(sum);
        }
    }
}
std::array<float, 2> TapeTable::read(RateSource source, double position,
                                     double rate) const SUPPLE_RT {
    const double whole = std::floor(position);
    const auto frame = static_cast<std::int64_t>(whole);
    if (rate == 1. && position == whole) return {source.at(0, frame), source.at(1, frame)};
    const auto phase = std::min(kPhases - 1, static_cast<int>((position - whole) * kPhases));
    const auto bank =
        std::clamp(static_cast<int>(std::ceil((boundedRate(rate) - 1.) * 4)), 0, kBanks - 1);
    const auto base = static_cast<std::size_t>((bank * kPhases + phase) * kTaps);
    double left = 0, right = 0;
    for (int tap = 0; tap < kTaps; ++tap) {
        const auto at = frame + tap - (kTaps / 2 - 1);
        const double c = double(coefficients_[base + static_cast<std::size_t>(tap)]);
        left += c * double(source.at(0, at));
        right += c * double(source.at(1, at));
    }
    return {static_cast<float>(left), static_cast<float>(right)};
}

struct StretchReader::State {
    signalsmith::stretch::SignalsmithStretch<float> stretch{kSeed};
    std::vector<float> history[2];
    std::array<float, kRateQuantum * 4 + 1> input[2];
    std::array<float, kRateQuantum> discard[2];
    double cursor = 0, primeRate = 1;
    std::uint32_t remaining = 0;
    bool positioned = false;
    explicit State(double sampleRate) {
        stretch.presetDefault(2, static_cast<float>(sampleRate));
        for (auto& channel : history)
            channel.resize(static_cast<std::size_t>(stretch.inputLatency()));
    }
    void run(RateSource source, double end, float* left, float* right) SUPPLE_RT {
        const auto from = static_cast<std::int64_t>(std::floor(cursor));
        const auto to = static_cast<std::int64_t>(std::floor(end));
        const int n = static_cast<int>(to - from);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                input[c][static_cast<std::size_t>(i)] = source.at(c, from + i);
        const float* inputs[] = {input[0].data(), input[1].data()};
        float* outputs[] = {left, right};
        stretch.process(inputs, n, outputs, kRateQuantum);
        cursor = end;
    }
};

StretchReader::StretchReader(double sampleRate) : state_(std::make_unique<State>(sampleRate)) {}
StretchReader::~StretchReader() = default;
StretchReader::StretchReader(StretchReader&&) noexcept = default;
StretchReader& StretchReader::operator=(StretchReader&&) noexcept = default;
void StretchReader::seek(RateSource source, double position, double rate) SUPPLE_RT {
    auto& s = *state_;
    s.positioned = true;
    s.stretch.reset(kSeed);
    s.primeRate = boundedRate(rate);
    const int latency = s.stretch.outputLatency();
    s.remaining = static_cast<std::uint32_t>((latency + kRateQuantum - 1) / kRateQuantum);
    // Round priming UP to canonical calls; shift the initial source location
    // back by the surplus so the first retained output still aligns to pos.
    s.cursor = position - (s.remaining * kRateQuantum - latency) * s.primeRate;
    const auto first = static_cast<std::int64_t>(std::floor(s.cursor));
    const int history = s.stretch.inputLatency();
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < history; ++i)
            s.history[c][static_cast<std::size_t>(i)] = source.at(c, first - history + i);
    const float* inputs[] = {s.history[0].data(), s.history[1].data()};
    s.stretch.seek(inputs, history, s.primeRate);
}
void StretchReader::prime(RateSource source, std::uint32_t quanta) SUPPLE_RT {
    auto& s = *state_;
    if (!s.positioned) return;
    while (quanta-- > 0 && s.remaining > 0) {
        s.run(source, s.cursor + kRateQuantum * s.primeRate, s.discard[0].data(),
              s.discard[1].data());
        --s.remaining;
    }
}
bool StretchReader::ready() const SUPPLE_RT { return state_->positioned && state_->remaining == 0; }
int StretchReader::lookahead() const SUPPLE_RT { return state_->stretch.outputLatency(); }
double StretchReader::inputPosition() const SUPPLE_RT { return state_->cursor; }
bool StretchReader::process(RateSource source, double end, std::span<float, kRateQuantum> left,
                            std::span<float, kRateQuantum> right) SUPPLE_RT {
    const double advance = end - state_->cursor;
    if (!ready() || !std::isfinite(advance) || advance < kRateQuantum * .25 ||
        advance > kRateQuantum * 4.)
        return false;
    state_->run(source, end, left.data(), right.data());
    return true;
}

}  // namespace supple::engine::detail
