// SPDX-License-Identifier: MIT
#include "utility/processor.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "supple/dsp/dsp.h"
namespace supple::utility {
void Processor::Ramp::set(double v, int samples, bool snap) {
    if (snap) {
        value = target = v;
        step = 0;
        remaining = 0;
    } else if (v != target) {
        target = v;
        remaining = samples;
        step = (v - value) / samples;
    }
}
double Processor::Ramp::next() {
    if (remaining && --remaining == 0)
        value = target;
    else if (remaining)
        value += step;
    return value;
}
double Processor::Filter::highpass(double x, double g) {
    constexpr double k = 1.4142135623730951;
    const double a1 = 1 / (1 + g * (g + k)), v1 = a1 * (a + g * (x - b)), v2 = b + g * v1;
    a = 2 * v1 - a;
    b = 2 * v2 - b;
    if (std::abs(a) < 1e-30) a = 0;
    if (std::abs(b) < 1e-30) b = 0;
    return x - k * v1 - v2;
}
void Processor::prepare(double sampleRate, uint32_t maxFrames, int channels) {
    (void)maxFrames;
    sr_ = sampleRate;
    channels_ = channels;
    dcAlpha_ = std::exp(-2 * std::numbers::pi * 5 / sr_);
    reset();
}
void Processor::reset() {
    started_ = false;
    for (auto& f : side_) f = {};
    for (int c = 0; c < 2; ++c) dcX_[c] = dcY_[c] = 0;
    inputPeak_ = outputPeak_ = ll_ = rr_ = lr_ = 0;
    configure(true);
}
void Processor::setParam(size_t index, double physical) {
    if (index >= kNumParams || (kParams[index].flags & params::kReadonly)) return;
    values_[index] = params::clampPhysical(kParams[index], physical);
    configure(!started_);
}
void Processor::setParams(const Values& v) {
    for (size_t i = 0; i < kNumParams; ++i)
        if (!(kParams[i].flags & params::kReadonly))
            values_[i] = params::clampPhysical(kParams[i], v[i]);
    configure(!started_);
}
void Processor::configure(bool snap) {
    const int shortRamp = std::max(1, static_cast<int>(std::lround(sr_ * 0.01))),
              longRamp = shortRamp * 2;
    double m[4] = {1, 0, 0, 1};
    switch (static_cast<int>(values_[kChannels])) {
        case 1:
            m[2] = 1;
            m[3] = 0;
            break;
        case 2:
            m[0] = 0;
            m[1] = 1;
            break;
        case 3:
            for (double& v : m) v = 0.5;
            break;
        case 4:
            m[0] = m[2] = 0.5;
            m[1] = m[3] = -0.5;
            break;
        default:
            break;
    }
    if (values_[kSwap] > 0.5) {
        std::swap(m[0], m[1]);
        std::swap(m[2], m[3]);
    }
    for (size_t i = 0; i < 4; ++i) matrix_[i].set(m[i], shortRamp, snap);
    invertL_.set(values_[kInvertLeft] > 0.5 ? -1 : 1, shortRamp, snap);
    invertR_.set(values_[kInvertRight] > 0.5 ? -1 : 1, shortRamp, snap);
    gain_.set(values_[kGain] <= -120 ? 0 : std::pow(10, values_[kGain] / 20), longRamp, snap);
    pan_.set(values_[kPan] / 100, longRamp, snap);
    width_.set(values_[kWidth] / 100, longRamp, snap);
    bass_.set(std::max(1.0, values_[kMonoBelow]), longRamp, snap);
    bassMix_.set(values_[kMonoBelow] > 0 ? 1 : 0, shortRamp, snap);
    dcMix_.set(values_[kDcBlock] > 0.5 ? 1 : 0, shortRamp, snap);
    mute_.set(values_[kMute] > 0.5 ? 0 : 1, shortRamp, snap);
}
template <class T>
void Processor::run(const T* const* in, T* const* out, uint32_t frames, bool telemetry) {
    dsp::DenormalGuard guard;
    started_ = true;
    for (uint32_t i = 0; i < frames; ++i) {
        const T originalL = in[0][i], originalR = channels_ == 2 ? in[1][i] : originalL;
        const double l = static_cast<double>(originalL), r = static_cast<double>(originalR);
        const double a = matrix_[0].next(), b = matrix_[1].next(), lowerLeft = matrix_[2].next(),
                     d = matrix_[3].next();
        double x = a == 1 && b == 0 ? l : a * l + b * r,
               y = lowerLeft == 0 && d == 1 ? r : lowerLeft * l + d * r;
        x *= invertL_.next();
        y *= invertR_.next();
        const double dm = dcMix_.next();
        double xy[2] = {x, y};
        for (int c = 0; c < 2; ++c) {
            double hp = xy[c] - dcX_[c] + dcAlpha_ * dcY_[c];
            dcX_[c] = xy[c];
            dcY_[c] = std::abs(hp) < 1e-30 ? 0 : hp;
            if (dm != 0) xy[c] += dm * (hp - xy[c]);
        }
        x = xy[0];
        y = xy[1];
        const double w = width_.next(), bm = bassMix_.next(), hz = bass_.next();
        // Skip the encode/decode at unity to preserve every input bit, including signed zero.
        if (w != 1 || bm != 0) {
            const double mid = (x + y) * 0.5, side = (x - y) * 0.5;
            const double g = std::tan(std::numbers::pi * std::min(hz, 0.45 * sr_) / sr_);
            double filtered = side_[1].highpass(side_[0].highpass(side, g), g);
            const double s = (side + bm * (filtered - side)) * w;
            x = mid + s;
            y = mid - s;
        } else {
            for (auto& f : side_) f = {};
        }
        const double pan = pan_.next(), gain = gain_.next() * mute_.next();
        x *= gain * (1 - std::max(0.0, pan));
        y *= gain * (1 + std::min(0.0, pan));
        // Copy at unity even with FTZ enabled: transparent audio retains subnormal bits.
        const bool unity = a == 1 && b == 0 && lowerLeft == 0 && d == 1 && invertL_.value == 1 &&
                           invertR_.value == 1 && dm == 0 && w == 1 && bm == 0 && pan == 0 &&
                           gain == 1;
        out[0][i] = unity ? originalL : static_cast<T>(x);
        if (channels_ == 2) out[1][i] = unity ? originalR : static_cast<T>(y);
        if (channels_ == 1) y = x;
        if (telemetry) {
            inputPeak_ = std::max(inputPeak_, std::max(std::abs(l), std::abs(r)));
            outputPeak_ = std::max(outputPeak_, std::max(std::abs(x), std::abs(y)));
            ll_ += x * x;
            rr_ += y * y;
            lr_ += x * y;
        }
    }
}
Processor::Telemetry Processor::takeTelemetry() {
    Telemetry t{20 * std::log10(std::max(1e-6, inputPeak_)),
                20 * std::log10(std::max(1e-6, outputPeak_)),
                ll_ > 0 && rr_ > 0 ? std::clamp(lr_ / std::sqrt(ll_ * rr_), -1.0, 1.0) : 0};
    inputPeak_ = outputPeak_ = ll_ = rr_ = lr_ = 0;
    return t;
}
void Processor::process(const float* const* in, float* const* out, uint32_t n) {
    run(in, out, n, false);
}
void Processor::process(const double* const* in, double* const* out, uint32_t n) {
    run(in, out, n, false);
}
void Processor::process(float* const* io, int n, bool telemetry) {
    run(io, io, static_cast<uint32_t>(n), telemetry);
}
void Processor::process(double* const* io, int n, bool telemetry) {
    run(io, io, static_cast<uint32_t>(n), telemetry);
}
}  // namespace supple::utility
