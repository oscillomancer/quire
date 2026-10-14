// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "eq/processor.h"
#include <algorithm>
#include <cmath>
#include "design.h"
#include "linear.h"
#include "supple/dsp/dsp.h"
namespace supple::eq {
namespace {
struct Smooth {
    double value = 0, target = 0, step = 0;
    int remaining = 0;
    void set(double v, int n) {
        if (v == target) return;
        target = v;
        remaining = n;
        step = (v - value) / n;
    }
    void snap(double v) {
        value = target = v;
        remaining = 0;
        step = 0;
    }
    bool tick() {
        if (!remaining) return false;
        if (--remaining == 0)
            value = target;
        else
            value += step;
        return true;
    }
};
struct State {
    double i1 = 0, i2 = 0;
    double sample(double x, const Section& s) {
        double y;
        if (s.matched) {
            y = s.b0 * x + i1;
            i1 = s.b1 * x - s.zA1 * y + i2;
            i2 = s.b2 * x - s.zA2 * y;
        } else if (s.first) {
            const double v = (x - i2) * s.g / (1 + s.g);
            const double low = v + i2;
            i2 = low + v;
            y = s.m0 * x + s.m2 * low;
        } else {
            const double v3 = x - i2, v1 = s.a1 * i1 + s.a2 * v3, v2 = i2 + s.a2 * i1 + s.a3 * v3;
            i1 = 2 * v1 - i1;
            i2 = 2 * v2 - i2;
            y = s.m0 * x + s.m1 * v1 + s.m2 * v2;
        }
        if (std::abs(i1) < 1e-30) i1 = 0;
        if (std::abs(i2) < 1e-30) i2 = 0;
        return y;
    }
};
struct Band {
    std::array<Smooth, 3> p;
    std::array<std::array<State, 8>, 2> states{};
    Design coefficients;
    int type = Bell, slope = 3, channel = Stereo;
    bool dirty = false;
    double wet = 0;
    bool switching = false;
    void clear() {
        for (auto& a : states)
            for (auto& s : a) s = {};
    }
    double run(double x, std::size_t c) {
        for (int k = 0; k < coefficients.n; ++k)
            x = states[c][static_cast<std::size_t>(k)].sample(
                x, coefficients.s[static_cast<std::size_t>(k)]);
        return x;
    }
};
}  // namespace
double bandMagnitudeDb(const Values& v, std::size_t b, double hz, double fs) {
    // Control/UI helper: reuse each band's design across a response sweep.
    struct Cache {
        Values values{};
        double rate = 0;
        std::array<Design, kBands> designs;
    };
    static thread_local Cache cache;
    if (cache.rate != fs || cache.values != v) {
        for (std::size_t band = 0; band < kBands; ++band) {
            const auto i = band * kBandParams;
            cache.designs[band] =
                v[i + Enabled] < .5 ? Design{}
                                    : design(static_cast<int>(v[i + Type]),
                                             std::min(v[i + Freq], fs * .49), v[i + Gain], v[i + Q],
                                             static_cast<int>(v[i + Slope]), fs, v[kPhase] == 1);
        }
        cache.values = v;
        cache.rate = fs;
    }
    return 20 * std::log10(std::max(1e-300, designMagnitude(cache.designs[b], hz, fs)));
}
double compensationDb(const Values& v, double fs) {
    double sum = 0;
    std::array<Design, kBands> designs;
    for (std::size_t b = 0; b < kBands; ++b) {
        const auto i = b * kBandParams;
        if (v[i + Enabled] >= .5)
            designs[b] =
                design(static_cast<int>(v[i + Type]), std::min(v[i + Freq], fs * .49), v[i + Gain],
                       v[i + Q], static_cast<int>(v[i + Slope]), fs, v[kPhase] == 1);
    }
    constexpr int n = 64;
    for (int j = 0; j < n; ++j) {
        const double hz = 20 * std::pow(std::min(20000., .49 * fs) / 20., (j + .5) / n);
        for (const auto& d : designs)
            sum += 20 * std::log10(std::max(1e-300, designMagnitude(d, hz, fs)));
    }
    return std::clamp(-sum / n, -24., 24.);
}
double magnitudeDb(const Values& v, double hz, double fs) {
    double db = v[kOutput];
    if (v[kPhase] == 2) {
        db += linearMagnitudeDb(v, hz, fs);
        if (v[kAutoGain] >= .5) db += compensationDb(v, fs);
        return db;
    }
    for (std::size_t b = 0; b < kBands; ++b) db += bandMagnitudeDb(v, b, hz, fs);
    if (v[kAutoGain] >= .5) db += compensationDb(v, fs);
    return db;
}
struct Processor::Impl {
    Values target = defaults();
    params::Table specs = table();
    std::array<Band, 8> bands;
    Smooth output, autoGain;
    Linear linear;
    double fs = 48000;
    int channels = 2, phase = 1, ramp = 960;
    bool snap = true, autoDirty = false;
    std::uint64_t sample = 0;
    void snapValues() {
        phase = static_cast<int>(target[kPhase]);
        output.snap(target[kOutput]);
        autoGain.snap(target[kAutoGain] >= .5 ? compensationDb(target, fs) : 0);
        for (std::size_t b = 0; b < kBands; ++b) {
            auto& band = bands[b];
            const auto i = b * kBandParams;
            band.type = static_cast<int>(target[i + Type]);
            band.slope = static_cast<int>(target[i + Slope]);
            band.channel = static_cast<int>(target[i + Channel]);
            band.dirty = false;
            band.p[0].snap(std::log2(target[i + Freq]));
            band.p[1].snap(target[i + Gain]);
            band.p[2].snap(std::log(target[i + Q]));
            band.wet = target[i + Enabled];
            band.switching = false;
            band.coefficients = design(band.type, std::min(target[i + Freq], fs * .49),
                                       target[i + Gain], target[i + Q], band.slope, fs, phase == 1);
        }
        autoDirty = false;
        snap = false;
    }
    void tickBands() {
        for (std::size_t b = 0; b < kBands; ++b) {
            auto& band = bands[b];
            const auto i = b * kBandParams;
            bool moved = false;
            for (auto& p : band.p) moved |= p.tick();
            band.dirty |= moved;
            autoDirty |= moved;
            const int type = static_cast<int>(target[i + Type]),
                      slope = static_cast<int>(target[i + Slope]),
                      channel = static_cast<int>(target[i + Channel]);
            band.switching = type != band.type || slope != band.slope || channel != band.channel;
            const double want = band.switching ? 0 : target[i + Enabled];
            const double delta = 1 / (fs * (band.switching ? .005 : .01));
            if (band.wet < want)
                band.wet = std::min(want, band.wet + delta);
            else if (band.wet > want)
                band.wet = std::max(want, band.wet - delta);
            if (band.switching && band.wet == 0) {
                band.type = type;
                band.slope = slope;
                band.channel = channel;
                band.clear();
                moved = true;
                band.dirty = true;
            }
            if ((phase == 1 ? (band.dirty && sample % 16 == 0) : moved)) {
                band.coefficients =
                    design(band.type, std::min(std::exp2(band.p[0].value), fs * .49),
                           band.p[1].value, std::exp(band.p[2].value), band.slope, fs, phase == 1);
                band.dirty = false;
                autoDirty = true;
            }
        }
        output.tick();
        autoGain.tick();
        if (sample % 128 == 0 && autoDirty) {
            double sum = 0;
            if (target[kAutoGain] >= .5) {
                for (int k = 0; k < 64; ++k) {
                    const double hz =
                        20 * std::pow(std::min(20000., .49 * fs) / 20., (k + .5) / 64);
                    for (std::size_t b = 0; b < kBands; ++b)
                        if (target[bandParam(b, Enabled)] >= .5)
                            sum +=
                                20 * std::log10(std::max(
                                         1e-300, designMagnitude(bands[b].coefficients, hz, fs)));
                }
            }
            autoGain.set(std::clamp(-sum / 64, -24., 24.), ramp);
            autoDirty = false;
        }
    }
    template <class T>
    void process(const T* const* in, T* const* out, std::uint32_t frames) {
        dsp::DenormalGuard guard;
        if (snap) snapValues();
        for (std::uint32_t f = 0; f < frames; ++f) {
            double l = static_cast<double>(in[0][f]),
                   r = channels == 2 ? static_cast<double>(in[1][f]) : l;
            if (!std::isfinite(l)) l = 0;
            if (!std::isfinite(r)) r = 0;
            tickBands();
            if (phase == 2)
                linear.sample(l, r);
            else
                for (auto& band : bands) {
                    if (band.wet == 0 || band.coefficients.n == 0) continue;
                    auto apply = [&](double x, std::size_t c) {
                        const double y = band.run(x, c);
                        return band.wet == 1 ? y : x + band.wet * (y - x);
                    };
                    switch (band.channel) {
                        case Left:
                            l = apply(l, 0);
                            break;
                        case Right:
                            if (channels == 2) r = apply(r, 1);
                            break;
                        case Mid: {
                            const double m = (l + r) * .5, s = (l - r) * .5, wet = apply(m, 0);
                            l = wet + s;
                            r = wet - s;
                            break;
                        }
                        case Side: {
                            if (channels == 1) break;
                            const double m = (l + r) * .5, s = (l - r) * .5, wet = apply(s, 1);
                            l = m + wet;
                            r = m - wet;
                            break;
                        }
                        default:
                            l = apply(l, 0);
                            r = apply(r, 1);
                            break;
                    }
                }
            const double db = output.value + autoGain.value;
            if (db != 0) {
                const double g = std::pow(10., db / 20.);
                l *= g;
                r *= g;
            }
            out[0][f] = static_cast<T>(l);
            if (channels == 2) out[1][f] = static_cast<T>(r);
            ++sample;
        }
    }
};
Processor::Processor() : impl_(std::make_unique<Impl>()) {}
Processor::~Processor() = default;
void Processor::prepare(double fs, std::uint32_t, int channels) {
    impl_->fs = std::clamp(fs, 22050., 384000.);
    impl_->channels = std::clamp(channels, 1, 2);
    impl_->ramp = static_cast<int>(.02 * impl_->fs);
    impl_->linear.prepare(impl_->fs);
    impl_->linear.publish(impl_->target, impl_->fs);
    reset();
}
void Processor::reset() SUPPLE_RT {
    for (auto& b : impl_->bands) b.clear();
    impl_->linear.reset();
    impl_->sample = 0;
    impl_->snap = true;
}
void Processor::setParam(std::size_t i, double v) SUPPLE_RT {
    if (i >= kParamCount) return;
    v = params::clampPhysical(impl_->specs[i], v);
    if (impl_->target[i] != v && i <= kAutoGain) impl_->autoDirty = true;
    impl_->target[i] = v;
    if (impl_->snap) return;
    if (i == kOutput)
        impl_->output.set(v, impl_->ramp);
    else if (i < kPhase) {
        const auto f = i % kBandParams;
        auto& b = impl_->bands[i / kBandParams];
        if (f == Freq) b.p[0].set(std::log2(v), impl_->ramp);
        if (f == Gain) b.p[1].set(v, impl_->ramp);
        if (f == Q) b.p[2].set(std::log(v), impl_->ramp);
    }
}
void Processor::setLinearValues(const Values& v) { impl_->linear.publish(v, impl_->fs); }
void Processor::collect() { impl_->linear.collect(); }
int Processor::latency() const { return impl_->target[kPhase] == 2 ? impl_->linear.latency() : 0; }
void Processor::process(const float* const* in, float* const* out, std::uint32_t frames) SUPPLE_RT {
    impl_->process(in, out, frames);
}
void Processor::process(const double* const* in, double* const* out,
                        std::uint32_t frames) SUPPLE_RT {
    impl_->process(in, out, frames);
}
}  // namespace supple::eq
