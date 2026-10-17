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
    rampFrames_ = rampAt_ = waitFrames_ = smoothing_ = 0;
}
double PlayrateClock::ramped() const SUPPLE_RT {
    return rampAt_ >= rampFrames_ ? target_
                                  : from_ + (target_ - from_) * (double(rampAt_) / rampFrames_);
}
void PlayrateClock::setRate(double rate, std::uint32_t frames, std::uint32_t smoothing) SUPPLE_RT {
    rate = boundedRate(rate);
    if (target_ == rate) return;
    // A smoothed scalar lags its ramp: the next ramp starts where the ramp is,
    // and the filter carries on from where the scalar is.
    from_ = smoothing_ > 0 && smoothing > 0 ? ramped() : scalar_;
    target_ = rate;
    rampFrames_ = frames;
    rampAt_ = 0;
    smoothing_ = smoothing;
    if (frames == 0 && smoothing == 0) scalar_ = target_;
}
void PlayrateClock::transition(double rate, double from, std::uint32_t frames,
                               std::uint32_t wait) SUPPLE_RT {
    scalar_ = from_ = boundedRate(from);
    target_ = boundedRate(rate);
    rampFrames_ = frames;
    rampAt_ = 0;
    waitFrames_ = wait;
    smoothing_ = 0;
    if (frames == 0 && wait == 0) scalar_ = target_;
}
void PlayrateClock::setEnvelope(Envelope envelope, const void* context) SUPPLE_RT {
    envelope_ = envelope;
    context_ = context;
}
double PlayrateClock::rate() const SUPPLE_RT {
    const double value = envelope_ ? envelope_(context_, timeline_) : scalar_;
    return std::isnan(value) ? scalar_ : boundedRate(value);
}
void PlayrateClock::advance(std::span<double> boundaries) SUPPLE_RT {
    if (boundaries.empty()) return;
    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        boundaries[i] = timeline_;
        timeline_ += rate();
        if (smoothing_ > 0) {
            if (rampAt_ < rampFrames_) ++rampAt_;
            const double ramp = ramped();
            scalar_ += (ramp - scalar_) / smoothing_;
            // Settled within 1e-5 (a fiftieth of a cent): the scalar is exact again.
            if (rampAt_ >= rampFrames_ && std::abs(ramp - scalar_) < 1e-5) scalar_ = ramp;
        } else if (waitFrames_ > 0) {
            if (--waitFrames_ == 0 && rampFrames_ == 0) scalar_ = target_;
        } else if (rampAt_ < rampFrames_) {
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
TapeReader::TapeReader(std::shared_ptr<const TapeTable> table, std::size_t channels)
    : table_(std::move(table)), channels_(channels), scratch_(channels * 512) {}
bool TapeReader::process(RateSource source, std::span<const double, kRateQuantum + 1> positions,
                         std::span<float* const> output) SUPPLE_RT {
    if (source.channels != channels_ || output.size() != channels_) return false;
    for (double p : positions)
        if (!std::isfinite(p)) return false;
    struct Phase {
        const float* coefficients;
        std::size_t first;
        bool unity;
    };
    std::array<Phase, kRateQuantum> phases;
    const auto begin = static_cast<std::int64_t>(std::floor(positions.front())) - 127;
    const auto count =
        static_cast<std::int64_t>(std::floor(positions[kRateQuantum - 1])) + 129 - begin;
    if (count <= 0 || count > 512) return false;
    for (int i = 0; i < kRateQuantum; ++i) {
        const double position = positions[static_cast<std::size_t>(i)];
        const double whole = std::floor(position);
        const double rate = positions[static_cast<std::size_t>(i + 1)] - position;
        if (!std::isfinite(position) || !std::isfinite(rate) || rate < .25 || rate > 4.)
            return false;
        const auto phase = std::min(TapeTable::kPhases - 1,
                                    static_cast<int>((position - whole) * TapeTable::kPhases));
        const auto bank =
            std::clamp(static_cast<int>(std::ceil((rate - 1.) * 4)), 0, TapeTable::kBanks - 1);
        const auto base =
            static_cast<std::size_t>((bank * TapeTable::kPhases + phase) * TapeTable::kTaps);
        phases[static_cast<std::size_t>(i)] = {
            table_->coefficients_.data() + base,
            static_cast<std::size_t>(static_cast<std::int64_t>(whole) - 127 - begin),
            rate == 1. && position == whole};
    }
    for (std::size_t c = 0; c < channels_; ++c) {
        const float* input =
            source.fetch(source.context, c, begin,
                         std::span(scratch_).subspan(c * 512, static_cast<std::size_t>(count)));
        for (int i = 0; i < kRateQuantum; ++i) {
            const auto& phase = phases[static_cast<std::size_t>(i)];
            const float* in = input + phase.first;
            if (phase.unity) {
                output[c][i] = in[127];
                continue;
            }
            // Independent lanes let the compiler vectorize without changing
            // the specified addition order or enabling fast-math.
            float sums[4]{};
            for (int tap = 0; tap < TapeTable::kTaps; tap += 4)
                for (int j = 0; j < 4; ++j) sums[j] += phase.coefficients[tap + j] * in[tap + j];
            output[c][i] = (sums[0] + sums[1]) + (sums[2] + sums[3]);
        }
    }
    return true;
}

struct StretchReader::State {
    signalsmith::stretch::SignalsmithStretch<float> stretch{kSeed};
    std::vector<float> history, input, discard;
    std::vector<const float*> inputs;
    std::vector<float*> outputs;
    std::size_t channels;
    double cursor = 0, primeRate = 1;
    std::uint32_t remaining = 0;
    bool positioned = false;
    State(double sampleRate, std::size_t channelCount) : channels(channelCount) {
        stretch.presetDefault(static_cast<int>(channels), static_cast<float>(sampleRate));
        history.resize(channels * static_cast<std::size_t>(stretch.inputLatency()));
        input.resize(channels * (kRateQuantum * 4 + 1));
        discard.resize(channels * kRateQuantum);
        inputs.resize(channels);
        outputs.resize(channels);
    }
    void run(RateSource source, double end, std::span<float* const> out) SUPPLE_RT {
        const auto from = static_cast<std::int64_t>(std::floor(cursor));
        const auto to = static_cast<std::int64_t>(std::floor(end));
        const auto n = static_cast<std::size_t>(to - from);
        for (std::size_t c = 0; c < channels; ++c)
            inputs[c] = source.fetch(source.context, c, from,
                                     std::span(input).subspan(c * (kRateQuantum * 4 + 1), n));
        stretch.process(inputs, static_cast<int>(n), out, kRateQuantum);
        cursor = end;
    }
};

StretchReader::StretchReader(double sampleRate, std::size_t channels)
    : state_(std::make_unique<State>(sampleRate, channels)) {}
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
    s.cursor =
        position + s.stretch.inputLatency() - (s.remaining * kRateQuantum - latency) * s.primeRate;
    const auto first = static_cast<std::int64_t>(std::floor(s.cursor));
    const int history = s.stretch.inputLatency();
    for (std::size_t c = 0; c < s.channels; ++c)
        s.inputs[c] =
            source.fetch(source.context, c, first - history,
                         std::span(s.history).subspan(c * static_cast<std::size_t>(history),
                                                      static_cast<std::size_t>(history)));
    s.stretch.seek(s.inputs, history, s.primeRate);
}
void StretchReader::prime(RateSource source, std::uint32_t quanta) SUPPLE_RT {
    auto& s = *state_;
    if (!s.positioned) return;
    while (quanta-- > 0 && s.remaining > 0) {
        for (std::size_t c = 0; c < s.channels; ++c)
            s.outputs[c] = s.discard.data() + c * kRateQuantum;
        s.run(source, s.cursor + kRateQuantum * s.primeRate, s.outputs);
        --s.remaining;
    }
}
bool StretchReader::primeTo(RateSource source, double end) SUPPLE_RT {
    auto& s = *state_;
    const double advance = end - s.cursor;
    if (!s.positioned || s.remaining == 0 || !std::isfinite(advance) || advance < 16 ||
        advance > 256)
        return false;
    for (std::size_t c = 0; c < s.channels; ++c) s.outputs[c] = s.discard.data() + c * kRateQuantum;
    s.run(source, end, s.outputs);
    --s.remaining;
    return true;
}
bool StretchReader::ready() const SUPPLE_RT { return state_->positioned && state_->remaining == 0; }
int StretchReader::lookahead() const SUPPLE_RT { return state_->stretch.outputLatency(); }
int StretchReader::analysisLookahead() const SUPPLE_RT { return state_->stretch.inputLatency(); }
double StretchReader::inputPosition() const SUPPLE_RT { return state_->cursor; }
bool StretchReader::process(RateSource source, double end,
                            std::span<float* const> output) SUPPLE_RT {
    const double advance = end - state_->cursor;
    if (!ready() || source.channels != state_->channels || output.size() != state_->channels ||
        !std::isfinite(advance) || advance < kRateQuantum * .25 || advance > kRateQuantum * 4.)
        return false;
    state_->run(source, end, output);
    return true;
}

}  // namespace supple::engine::detail
