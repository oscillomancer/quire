// SPDX-License-Identifier: MIT
#include "gate/processor.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "supple/dsp/dsp.h"
namespace supple::gate {
void Processor::Ramp::set(double v, uint32_t n, bool snap) {
    if (snap) {
        value = target = v;
        left = 0;
        step = 0;
    } else if (v != target) {
        target = v;
        left = n;
        step = (v - value) / n;
    }
}
double Processor::Ramp::next() {
    if (left) {
        value += step;
        if (--left == 0) value = target;
    }
    return value;
}
double Processor::Filter::run(double x, double g, bool high) {
    constexpr double k = 1.4142135623730951;
    const double a1 = 1 / (1 + g * (g + k)), v1 = a1 * (a + g * (x - b)), v2 = b + g * v1;
    a = 2 * v1 - a;
    b = 2 * v2 - b;
    if (std::abs(a) < 1e-30) a = 0;
    if (std::abs(b) < 1e-30) b = 0;
    return high ? x - k * v1 - v2 : v2;
}
void Processor::prepare(double fs, uint32_t maxFrames, int channels) {
    (void)maxFrames;
    sr_ = std::isfinite(fs) ? std::clamp(fs, 1000., 768000.) : 48000;
    channels_ = std::clamp(channels, 1, 2);
    delay_.resize(static_cast<size_t>(std::ceil(sr_ * .005)) + 1);
    reset();
}
void Processor::reset() SUPPLE_RT {
    started_ = false;
    write_ = grid_ = 0;
    for (auto& s : delay_) s = {};
    for (int c = 0; c < 2; ++c) {
        hp_[c] = {};
        lp_[c] = {};
        channel_[c] = {};
        channel_[c].gate = channel_[c].expand = values_[kRange];
    }
    level_ = -120;
    gr_ = values_[kRange];
    open_ = false;
    configure(true);
}
void Processor::setParam(size_t i, double v) SUPPLE_RT {
    if (i >= kGr || (i == kLookahead && started_)) return;
    values_[i] = params::clampPhysical(table()[i], v);
    configure(!started_);
    if (!started_)
        for (auto& c : channel_) c.gate = c.expand = values_[kRange];
}
void Processor::setParams(std::span<const double> v) SUPPLE_RT {
    for (size_t i = 0; i < kGr && i < v.size(); ++i) {
        if (i == kLookahead && started_) continue;
        values_[i] = params::clampPhysical(table()[i], v[i]);
    }
    configure(!started_);
    if (!started_)
        for (auto& c : channel_) c.gate = c.expand = values_[kRange];
}
void Processor::configure(bool snap) {
    for (size_t i = 0; i < kGr; ++i) {
        double v = values_[i];
        if (i == kScHighpass || i == kScLowpass) v = std::log(v);
        const auto samples = static_cast<uint32_t>(
            std::max(1., std::round(sr_ * (table()[i].type == params::Type::Float ? .02 : .01))));
        ramp_[i].set(v, samples, snap);
    }
    const auto n = static_cast<uint32_t>(std::max(1., std::round(sr_ * .01)));
    hpMix_.set(values_[kScHighpass] > 10 ? 1 : 0, n, snap);
    lpMix_.set(values_[kScLowpass] < 20000 ? 1 : 0, n, snap);
    if (snap) {
        delaySamples_ = lookaheadSamples(values_[kLookahead], sr_);
        grid_ = 0;
    }
}
Telemetry Processor::telemetry() const SUPPLE_RT { return {level_, gr_, open_}; }
template <class T>
void Processor::run(const T* const* in, T* const* out, uint32_t frames, const T* const* key) {
    dsp::DenormalGuard denorm;
    started_ = true;
    const auto safe = [](auto x) {
        const double v = static_cast<double>(x);
        return std::isfinite(v) ? std::clamp(v, -1e8, 1e8) : 0.;
    };
    for (uint32_t n = 0; n < frames; ++n) {
        for (size_t i = 0; i < kGr; ++i) ramp_[i].next();
        const double hm = hpMix_.next(), lm = lpMix_.next();
        if (grid_++ % 16 == 0) {
            hpG_ = std::tan(std::numbers::pi *
                            std::min(std::exp(ramp_[kScHighpass].value), .45 * sr_) / sr_);
            lpG_ = std::tan(std::numbers::pi *
                            std::min(std::exp(ramp_[kScLowpass].value), .45 * sr_) / sr_);
            attack_ = std::exp(-1 / (ramp_[kAttack].value * .001 * sr_));
            release_ = std::exp(-1 / (ramp_[kRelease].value * .001 * sr_));
        }
        const double main[2] = {safe(in[0][n]), safe(in[channels_ == 1 ? 0 : 1][n])};
        double filtered[2];
        for (int c = 0; c < 2; ++c) {
            const double ext = key && key[c] ? safe(key[c][n]) : 0;
            double x = main[c] + ramp_[kSidechain].value * (ext - main[c]);
            const double high = hp_[c].run(x, hpG_, true);
            x += hm * (high - x);
            const double low = lp_[c].run(x, lpG_, false);
            filtered[c] = x + lm * (low - x);
        }
        const double peak[2] = {std::abs(filtered[0]), std::abs(filtered[1])},
                     maximum = std::max(peak[0], peak[1]), link = ramp_[kStereoLink].value / 100;
        delay_[write_] = {main[0], main[1], filtered[0], filtered[1]};
        const auto delayed = delay_[(write_ + delay_.size() - delaySamples_) % delay_.size()];
        write_ = (write_ + 1) % delay_.size();
        gr_ = 0;
        level_ = -120;
        open_ = false;
        for (int c = 0; c < 2; ++c) {
            const double level =
                20 * std::log10(std::max(1e-12, peak[c] + link * (maximum - peak[c])));
            auto& state = channel_[c];
            const double threshold = ramp_[kThreshold].value, range = ramp_[kRange].value;
            const auto hold =
                static_cast<uint32_t>(std::max(static_cast<double>(delaySamples_ + 1),
                                               std::round(ramp_[kHold].value * .001 * sr_)));
            if (level > threshold && !state.open) {
                state.open = true;
                state.deadline = delaySamples_;
                state.gateStart = state.gate;
                state.expandStart = state.expand;
            }
            if (level >= threshold - ramp_[kHysteresis].value)
                state.hold = hold;
            else if (state.hold)
                --state.hold;
            else
                state.open = false;
            const double targets[2] = {
                state.open ? 0 : range,
                state.open ? 0 : expansionDb(level, threshold, ramp_[kRatio].value, range)};
            double* gains[2] = {&state.gate, &state.expand};
            for (int j = 0; j < 2; ++j) {
                double& g = *gains[j];
                const double alpha = targets[j] > g ? attack_ : release_;
                g = targets[j] + alpha * (g - targets[j]);
                if (std::abs(g - targets[j]) < 1e-4) g = targets[j];
            }
            if (state.deadline) {
                // Normalize the one-pole opening to the available preview time.
                // This reaches unity before the delayed onset without an end-of-ramp jump.
                const double time = ramp_[kAttack].value * .001 * sr_;
                const double progress = static_cast<double>(delaySamples_ - state.deadline + 1);
                const double fraction = -std::expm1(-progress / time) /
                                        -std::expm1(-static_cast<double>(delaySamples_) / time);
                state.gate = state.gateStart * (1 - fraction);
                state.expand = state.expandStart * (1 - fraction);
                --state.deadline;
            }
            const double db = state.gate + ramp_[kMode].value * (state.expand - state.gate);
            const double amplitude = range <= -80 && db <= -79.9999 ? 0 : std::pow(10, db / 20);
            double wet = delayed[c] * amplitude;
            wet += ramp_[kScListen].value * (delayed[c + 2] - wet);
            const double mix = ramp_[kMix].value / 100,
                         output = std::pow(10, ramp_[kOutput].value / 20);
            if (c < channels_)
                out[c][n] = static_cast<T>((delayed[c] + mix * (wet - delayed[c])) * output);
            gr_ = std::min(gr_, db);
            level_ = std::max(level_, level);
            open_ = open_ || state.open;
        }
    }
}
void Processor::process(const float* const* in, float* const* out, uint32_t n,
                        const float* const* key) SUPPLE_RT {
    run(in, out, n, key);
}
void Processor::process(const double* const* in, double* const* out, uint32_t n,
                        const double* const* key) SUPPLE_RT {
    run(in, out, n, key);
}
void Processor::process(float* const* io, int n, bool) SUPPLE_RT {
    if (n > 0) run(io, io, static_cast<uint32_t>(n), static_cast<const float* const*>(nullptr));
}
void Processor::process(double* const* io, int n, bool) SUPPLE_RT {
    if (n > 0) run(io, io, static_cast<uint32_t>(n), static_cast<const double* const*>(nullptr));
}
}  // namespace supple::gate
