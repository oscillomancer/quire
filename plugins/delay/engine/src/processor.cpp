// SPDX-License-Identifier: MIT
#include "delay/processor.h"
#include <algorithm>
#include <cmath>
#include "supple/dsp/dsp.h"
namespace supple::delay {
namespace {
constexpr double pi = 3.14159265358979323846;
double flush(double x) { return std::abs(x) < 1e-30 ? 0 : x; }
double safe(double x) { return std::isfinite(x) ? std::clamp(x, -1e8, 1e8) : 0; }
double db(double x) { return 20 * std::log10(std::max(1e-6, std::abs(x))); }
double cubic(double a, double b, double c, double d, double t) {
    return b + .5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}
bool logarithmic(size_t i) { return i == kHighpass || i == kLowpass || i == kModRate; }
}  // namespace
void Processor::Ramp::set(double v, uint32_t n) {
    target = v;
    if (n == 0) {
        value = v;
        left = 0;
        step = 0;
    } else {
        left = n;
        step = (v - value) / n;
    }
}
double Processor::Ramp::next() {
    if (left) {
        value += step;
        if (!--left) value = target;
    }
    return value;
}
void Processor::Filter::tune(double hz, double fs) {
    g = std::tan(pi * std::min(hz, .45 * fs) / fs);
    a1 = 1 / (1 + g * (g + std::sqrt(2.)));
    a2 = g * a1;
    a3 = g * a2;
}
double Processor::Filter::tick(double x, bool high) {
    const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
    s1 = flush(2 * v1 - s1);
    s2 = flush(2 * v2 - s2);
    return high ? x - std::sqrt(2.) * v1 - v2 : v2;
}
double Processor::Line::random() {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return double(seed) / 4294967295. * 2 - 1;
}
double Processor::Line::read(double delay) const {
    const double pos = double(write) - delay;
    const auto base = static_cast<int64_t>(std::floor(pos));
    const auto size = static_cast<int64_t>(buffer.size());
    const auto get = [&](int64_t i) {
        i = (i % size + size) % size;
        return double(buffer[size_t(i)]);
    };
    return cubic(get(base - 1), get(base), get(base + 1), get(base + 2), pos - std::floor(pos));
}
double Processor::Line::frozenRead() const {
    const double pos = double(frozenPos) - frozenFraction;
    const auto base = static_cast<int64_t>(std::floor(pos)),
               size = static_cast<int64_t>(frozenLength);
    const auto get = [&](int64_t i) {
        i = (i % size + size) % size;
        return double(buffer[(frozenStart + size_t(i)) % buffer.size()]);
    };
    return cubic(get(base - 1), get(base), get(base + 1), get(base + 2), pos - std::floor(pos));
}
double Processor::encode(size_t i, double v) const { return logarithmic(i) ? std::log(v) : v; }
double Processor::decode(size_t i, double v) const { return logarithmic(i) ? std::exp(v) : v; }
bool Processor::prepare(double rate, uint32_t maxFrames, int channels) {
    if (!std::isfinite(rate) || rate < 1000 || rate > 768000 || !maxFrames ||
        (channels != 1 && channels != 2))
        return false;
    fs_ = rate;
    channels_ = channels;
    for (auto& line : lines_) line.buffer.assign(size_t(std::ceil(fs_ * 5.004)) + 8, 0);
    glideAlpha_ = std::exp(-1 / (fs_ * .1));
    attack_ = std::exp(-1 / (fs_ * .001));
    release_ = std::exp(-1 / (fs_ * .25));
    reset();
    return true;
}
void Processor::reset() {
    started_ = false;
    sample_ = 0;
    phase_ = 0;
    env_ = -120;
    freezeTarget_ = values_[kFreeze] != 0;
    const auto times = lineTimes(values_, tempo_);
    for (size_t i = 0; i < kNumParams; ++i) ramps_[i].set(encode(i, values_[i]), 0);
    for (size_t i = 0; i < 3; ++i) modes_[i].set(size_t(values_[kMode]) == i ? 1 : 0, 0);
    for (size_t i = 0; i < 2; ++i) {
        auto& l = lines_[i];
        std::fill(l.buffer.begin(), l.buffer.end(), 0);
        l.write = 0;
        l.captured = false;
        l.glide1 = l.glide2 = times[i] * fs_ / 1000;
        l.hp = {};
        l.lp = {};
        l.hp.tune(values_[kHighpass], fs_);
        l.lp.tune(values_[kLowpass], fs_);
        l.seed = 0x9e3779b9u + uint32_t(i) * 0x6d2b79f5u;
        l.randomA = l.random();
        l.randomB = l.random();
    }
    telemetry_ = {times, tempo_, -120, -120, 0};
}
void Processor::setTempo(double bpm) {
    tempo_ = std::isfinite(bpm) && bpm > 0 ? bpm : 120;
    if (!started_) {
        const auto t = lineTimes(values_, tempo_);
        for (size_t i = 0; i < 2; ++i) lines_[i].glide1 = lines_[i].glide2 = t[i] * fs_ / 1000;
    }
}
void Processor::setParam(size_t i, double v) {
    if (i >= kNumParams || !std::isfinite(v)) return;
    const auto& p = table()[i];
    v = std::clamp(v, p.min, p.max);
    if (p.type != params::Type::Float) v = std::round(v);
    if (v == values_[i]) return;
    values_[i] = v;
    if (i == kFreeze) {
        freezeTarget_ = v != 0;
    }
    const uint32_t count =
        started_
            ? uint32_t(std::max(1., std::round(fs_ * (i == kFreeze || i == kMode ? .01 : .02))))
            : 0;
    ramps_[i].set(encode(i, v), count);
    if (i == kMode)
        for (size_t m = 0; m < 3; ++m) modes_[m].set(m == size_t(v) ? 1 : 0, count);
    if (!started_) {
        const auto t = lineTimes(values_, tempo_);
        for (size_t c = 0; c < 2; ++c) lines_[c].glide1 = lines_[c].glide2 = t[c] * fs_ / 1000;
    }
}
void Processor::setParams(std::span<const double> v) {
    for (size_t i = 0; i < std::min(v.size(), values_.size()); ++i) setParam(i, v[i]);
}
void Processor::process(const float* const* in, float* const* out, uint32_t n) {
    render(in, out, n);
}
void Processor::process(const double* const* in, double* const* out, uint32_t n) {
    render(in, out, n);
}
template <class T>
void Processor::render(const T* const* in, T* const* out, uint32_t frames) {
    if (lines_[0].buffer.empty()) return;
    dsp::DenormalGuard guard;
    if (!started_) {
        for (auto& l : lines_) {
            l.hp.tune(values_[kHighpass], fs_);
            l.lp.tune(values_[kLowpass], fs_);
        }
    }
    started_ = true;
    for (uint32_t n = 0; n < frames; ++n) {
        Values v{};
        for (size_t i = 0; i < kNumParams; ++i) v[i] = decode(i, ramps_[i].next());
        std::array<double, 3> modes{};
        for (size_t i = 0; i < 3; ++i) modes[i] = modes_[i].next();
        const double frozen = v[kFreeze];
        const auto target = lineTimes(values_, tempo_);
        double input[2]{safe(in && in[0] ? double(in[0][n]) : 0), 0};
        input[1] = channels_ == 2 ? safe(in && in[1] ? double(in[1][n]) : 0) : input[0];
        const double level = db(std::max(std::abs(input[0]), std::abs(input[1])));
        env_ = flush(level + (env_ - level) * (level > env_ ? attack_ : release_));
        double raw[2]{}, echo[2]{};
        for (size_t c = 0; c < 2; ++c) {
            auto& l = lines_[c];
            if (!freezeTarget_) {
                l.glide1 =
                    target[c] * fs_ / 1000 + (l.glide1 - target[c] * fs_ / 1000) * glideAlpha_;
                l.glide2 = l.glide1 + (l.glide2 - l.glide1) * glideAlpha_;
            }
            if ((sample_ & 15) == 0) {
                l.hp.tune(v[kHighpass], fs_);
                l.lp.tune(v[kLowpass], fs_);
            }
            const double smooth = phase_ * phase_ * (3 - 2 * phase_);
            const double wow = v[kModDepth] / 100 * .003 * fs_ *
                               (.8 * std::sin(2 * pi * (phase_ + .25 * c)) +
                                .2 * (l.randomA + (l.randomB - l.randomA) * smooth));
            const double delay = std::max(
                2., l.glide2 + std::clamp(wow, -.45 * l.glide2, .45 * l.glide2) * (1 - frozen));
            if (l.captured && frozen < 1) {
                l.write = (l.frozenStart + l.frozenPos + l.frozenLength) % l.buffer.size();
                l.captured = false;
            }
            if (!l.captured && frozen == 1) {
                l.frozenLength = std::max(size_t(1), size_t(std::llround(l.glide2)));
                l.frozenStart = (l.write + l.buffer.size() - l.frozenLength) % l.buffer.size();
                l.frozenPos = 0;
                l.frozenFraction = l.glide2 - double(l.frozenLength);
                l.captured = true;
            }
            raw[c] = l.captured ? l.frozenRead() : l.read(delay);
            const double filtered = l.lp.tick(l.hp.tick(raw[c], true), false);
            const double gain = std::pow(10., v[kDrive] / 20);
            const double driven = v[kDrive] == 0 ? filtered : std::tanh(gain * filtered) / gain;
            echo[c] = (1 - frozen) * safetyLimit(driven) + frozen * raw[c];
        }
        const double mono = (input[0] + input[1]) * .5, fb = v[kFeedback] / 100;
        for (size_t c = 0; c < 2; ++c) {
            auto& l = lines_[c];
            if (l.captured) {
                l.frozenPos = (l.frozenPos + 1) % l.frozenLength;
                continue;
            }
            const double dry = modes[0] * input[c] + (c == 0 ? (modes[1] + modes[2]) * mono : 0);
            const double normal =
                modes[0] * echo[c] + modes[1] * echo[1 - c] + (c == 0 ? modes[2] * echo[0] : 0);
            const double write = (1 - frozen) * (dry + fb * normal) + frozen * raw[c];
            l.buffer[l.write] = static_cast<float>(flush(write));
            l.write = (l.write + 1) % l.buffer.size();
        }
        double wet[2]{echo[0] * (modes[0] + modes[1] + modes[2]),
                      (modes[0] + modes[1]) * echo[1] + modes[2] * echo[0]};
        const double mid = (wet[0] + wet[1]) * .5, side = (wet[0] - wet[1]) * .5 * v[kWidth] / 100;
        if (v[kWidth] != 100) {
            wet[0] = mid + side;
            wet[1] = mid - side;
        }
        const double duck = v[kDuck] * std::clamp((env_ + 60) / 30, 0., 1.),
                     attenuation = std::pow(10., -duck / 20);
        const double mix = v[kMix] / 100, output = std::pow(10., v[kOutput] / 20);
        for (int c = 0; c < channels_; ++c)
            if (out && out[c])
                out[c][n] =
                    static_cast<T>(((1 - mix) * input[c] + mix * wet[c] * attenuation) * output);
        telemetry_ = {{lines_[0].glide2 * 1000 / fs_, lines_[1].glide2 * 1000 / fs_},
                      tempo_,
                      level,
                      db(std::max(std::abs(wet[0]), std::abs(wet[1]))),
                      -duck};
        phase_ += v[kModRate] / fs_;
        if (phase_ >= 1) {
            phase_ -= 1;
            for (auto& l : lines_) {
                l.randomA = l.randomB;
                l.randomB = l.random();
            }
        }
        ++sample_;
    }
}
}  // namespace supple::delay
