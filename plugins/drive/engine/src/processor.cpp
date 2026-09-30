// SPDX-License-Identifier: MIT
#include "drive/processor.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "supple/dsp/dsp.h"
namespace supple::drive {
namespace {
double rational(int curve, double x) {
    const double a = curve == 0 ? 1.2 : curve == 1 ? .6 : 1.0;
    const double c = curve == 0 ? 0 : curve == 1 ? .4 : .5;
    const double t = x / a, r = std::hypot(1.0, t), u = t / r;
    return a * (u + c * u * u * u);
}
double rationalIntegral(int curve, double x) {
    const double a = curve == 0 ? 1.2 : curve == 1 ? .6 : 1.0;
    const double c = curve == 0 ? 0 : curve == 1 ? .4 : .5;
    const double t = x / a, r = std::hypot(1.0, t);
    // r-1 evaluated without cancellation close to the origin.
    const double d = t * (t / (r + 1));
    return a * a * d * (1 + c - c / r);
}
double db(double x) { return x > 1e-6 ? std::min(40.0, 20 * std::log10(x)) : -120; }
double flush(double x) { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace
double curveValue(int curve, double x) SUPPLE_RT {
    if (curve < 3) return rational(curve, x);
    if (curve == 3) return std::tanh(x);
    if (curve == 4) return .35 * rational(0, x) + .65 * std::sin(x);
    return std::clamp(x, -1.0, 1.0);
}
double curveIntegral(int curve, double x) SUPPLE_RT {
    if (curve < 3) return rationalIntegral(curve, x);
    if (curve == 3) {
        const double a = std::abs(x);
        return a + std::log1p(std::exp(-2 * a)) - std::log(2.0);
    }
    if (curve == 4) {
        const double s = std::sin(x * .5);
        return .35 * rationalIntegral(0, x) + 1.3 * s * s;
    }
    const double a = std::abs(x);
    return a <= 1 ? .5 * x * x : a - .5;
}
double curveAdaa(int curve, double x, double previous) SUPPLE_RT {
    if (std::abs(x - previous) < 1e-5) return curveValue(curve, (x + previous) * .5);
    return (curveIntegral(curve, x) - curveIntegral(curve, previous)) / (x - previous);
}
void Processor::Ramp::set(double v, int samples, bool snap) SUPPLE_RT {
    if (snap) {
        value = target = v;
        remaining = 0;
        step = 0;
        return;
    }
    if (v == target) return;
    target = v;
    remaining = std::max(1, samples);
    step = (target - value) / remaining;
}
double Processor::Ramp::next() SUPPLE_RT {
    if (remaining && --remaining == 0)
        value = target;
    else if (remaining)
        value += step;
    return value;
}
double Processor::Tilt::process(double x, double b0, double b1, double a1) SUPPLE_RT {
    const double y = b0 * x + b1 * previousX - a1 * previousY;
    previousX = flush(x);
    previousY = flush(y);
    return y;
}
void Processor::prepare(double sampleRate, uint32_t maxFrames, int channels) {
    sr_ = std::clamp(sampleRate, 22050.0, 384000.0);
    channels_ = std::clamp(channels, 1, 2);
    factor_ = 1 << static_cast<int>(values_[kOversampling]);
    for (auto& o : oversamplers_) o.prepare(factor_, maxFrames);
    dcAlpha_ = std::exp(-2 * std::numbers::pi * 5 / (sr_ * factor_));
    constexpr double sigma = 0.12589254117941673;
    // Deterministic Gaussian quadrature on [-8,8], 2048 trapezoids.
    for (int c = 0; c < 6; ++c)
        for (int d = 0; d < 145; ++d) {
            const double gain = std::pow(10.0, d * .25 / 20);
            double power = 0;
            for (int j = 0; j <= 2048; ++j) {
                const double z = -8 + j / 128.0;
                const double y = curveValue(c, z * sigma * gain);
                power += y * y * std::exp(-.5 * z * z) / std::sqrt(2 * std::numbers::pi) / 128.0 *
                         (j == 0 || j == 2048 ? .5 : 1);
            }
            gainTable_[c][d] = sigma / std::sqrt(power);
        }
    reset();
}
void Processor::reset() SUPPLE_RT {
    for (auto& o : oversamplers_) o.reset();
    for (auto& d : dry_) d.fill(0);
    for (auto& d : wetPad_) d.fill(0);
    for (int c = 0; c < 2; ++c) {
        pre_[c] = {};
        post_[c] = {};
        previous_[c] = adaaPrevious_[c] = dcX_[c] = dcY_[c] = 0;
    }
    dryHead_ = padHead_ = 0;
    inputPeak_ = outputPeak_ = 0;
    started_ = false;
    configure(true);
}
void Processor::configure(bool snap) SUPPLE_RT {
    const int ramp = static_cast<int>(std::ceil(sr_ * .02)),
              fade = static_cast<int>(std::ceil(sr_ * .01));
    drive_.set(values_[kDrive], ramp, snap);
    bias_.set(values_[kBias] / 100, ramp, snap);
    tone_.set(values_[kTone], ramp, snap);
    mix_.set(values_[kMix] / 100, ramp, snap);
    output_.set(values_[kOutput], ramp, snap);
    autoGain_.set(values_[kAutoGain], fade, snap);
    for (int c = 0; c < 6; ++c)
        weights_[c].set(c == static_cast<int>(values_[kCurve]) ? 1 : 0, fade, snap);
}
void Processor::setParam(size_t index, double physical) SUPPLE_RT {
    if (index >= kNumParams || !std::isfinite(physical) || (kParams[index].flags & pa::kReadonly))
        return;
    auto& p = kParams[index];
    physical = std::clamp(physical, p.min, p.max);
    if (p.type != pa::Type::Float) physical = std::round(physical);
    values_[index] = physical;
    configure(!started_);
}
void Processor::setParams(const Values& v) SUPPLE_RT {
    for (size_t i = 0; i < v.size(); ++i) setParam(i, v[i]);
}
double Processor::compensation(int curve, double driveDb) const SUPPLE_RT {
    const double x = std::clamp(driveDb, 0.0, 36.0) * 4;
    const int i = std::min(143, static_cast<int>(x));
    return gainTable_[curve][i] + (x - i) * (gainTable_[curve][i + 1] - gainTable_[curve][i]);
}
Processor::Telemetry Processor::takeTelemetry() SUPPLE_RT {
    Telemetry t{db(inputPeak_), db(outputPeak_)};
    inputPeak_ = outputPeak_ = 0;
    return t;
}
template <class T>
void Processor::run(const T* const* in, T* const* out, uint32_t frames, bool telemetry) SUPPLE_RT {
    dsp::DenormalGuard guard;
    started_ = true;
    for (uint32_t n = 0; n < frames; ++n) {
        const double d = drive_.next(), gain = std::pow(10.0, d / 20), bias = bias_.next(),
                     tone = tone_.next();
        const double mix = mix_.next(), output = std::pow(10.0, output_.next() / 20),
                     automatic = autoGain_.next();
        double w[6], comp[6];
        for (int i = 0; i < 6; ++i) {
            w[i] = weights_[i].next();
            comp[i] = 1 + automatic * (compensation(i, d) - 1);
        }
        const double a = std::pow(10.0, 2 * tone / 20),
                     k = std::tan(std::numbers::pi * 1000 / (sr_ * factor_));
        const double b0 = (a + k) / (1 + a * k), b1 = (-a + k) / (1 + a * k),
                     a1 = (-1 + a * k) / (1 + a * k);
        for (int c = 0; c < channels_; ++c) {
            const double raw = std::isfinite(in[c][n]) ? static_cast<double>(in[c][n]) : 0;
            dry_[c][dryHead_] = raw;
            const double dry = dry_[c][(dryHead_ + 1024 - latency()) % 1024];
            std::array<double, 8> hi{};
            oversamplers_[c].upsample(raw, hi);
            for (int s = 0; s < factor_; ++s) {
                const double x = pre_[c].process(hi[s], b0, b1, a1) * gain + bias;
                double y = 0;
                for (int i = 0; i < 6; ++i)
                    if (w[i] > 0) y += w[i] * comp[i] * curveAdaa(i, x, previous_[c]);
                previous_[c] = x;
                const double delayed = .5 * (y + adaaPrevious_[c]);
                adaaPrevious_[c] = y;
                // ADAA (1/2 high sample) plus averaging (1/2) and F-1 pure
                // high samples complete one base sample of nonlinear delay.
                wetPad_[c][(padHead_ + s) % 8] = delayed;
                y = wetPad_[c][(padHead_ + s + 8 - (factor_ - 1)) % 8];
                y = post_[c].process(y, 1 / b0, a1 / b0, b1 / b0);
                const double dc = y - dcX_[c] + dcAlpha_ * dcY_[c];
                dcX_[c] = flush(y);
                dcY_[c] = flush(dc);
                hi[s] = dc;
            }
            const double wet = oversamplers_[c].downsample(hi);
            const double y = (mix == 0 ? dry : mix == 1 ? wet : dry + mix * (wet - dry)) * output;
            out[c][n] = static_cast<T>(y);
            if (telemetry) {
                inputPeak_ = std::max(inputPeak_, std::abs(raw));
                outputPeak_ = std::max(outputPeak_, std::abs(y));
            }
        }
        dryHead_ = (dryHead_ + 1) % 1024;
        padHead_ = (padHead_ + factor_) % 8;
    }
}
void Processor::process(const float* const* in, float* const* out, uint32_t n) SUPPLE_RT {
    run(in, out, n, false);
}
void Processor::process(const double* const* in, double* const* out, uint32_t n) SUPPLE_RT {
    run(in, out, n, false);
}
void Processor::process(float* const* io, int n, bool telemetry) SUPPLE_RT {
    run(io, io, static_cast<uint32_t>(std::max(0, n)), telemetry);
}
void Processor::process(double* const* io, int n, bool telemetry) SUPPLE_RT {
    run(io, io, static_cast<uint32_t>(std::max(0, n)), telemetry);
}
}  // namespace supple::drive
