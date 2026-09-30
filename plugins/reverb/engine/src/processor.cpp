// SPDX-License-Identifier: MIT
#include "reverb/processor.h"
#include <algorithm>
#include <bit>
#include <exception>
#include <numbers>
namespace supple::reverb {
namespace {
constexpr std::uint32_t kFilterControls = (1u << kSize) | (1u << kDecay) | (1u << kLowDecay) |
                                          (1u << kHighDecay) | (1u << kCrossover) |
                                          (1u << kDampingFreq) | (1u << kLowCut) | (1u << kHighCut);
constexpr double pi = std::numbers::pi;
double flush(double v) SUPPLE_RT { return std::abs(v) < 1e-30 ? 0 : v; }
// Independent fixed stream; no shared mutable RNG or platform distribution.
double random(std::uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<double>(seed) / 4294967296.;
}
constexpr std::array<int, 16> inL = {1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1};
constexpr std::array<int, 16> inR = {1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1, 1, -1, -1, -1, -1};
constexpr std::array<int, 16> outL = {1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1};
constexpr std::array<int, 16> outR = {1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1};
}  // namespace
Processor::Processor() : target_(defaults()), current_(target_) {
    std::uint32_t seed = 0x4e415645;
    for (std::size_t i = 0; i < 16; ++i)
        baseMs_[i] =
            25 * std::pow(95. / 25, static_cast<double>(i) / 15) * (.95 + .1 * random(seed));
    for (std::size_t c = 0; c < 2; ++c) {
        double sum = 0;
        for (std::size_t i = 0; i < 12; ++i) {
            tapMs_[c][i] = 5 + 75 * (static_cast<double>(i) + .6 * random(seed)) / 12;
            tapGain_[c][i] = std::exp(-tapMs_[c][i] / 35);
            sum += std::abs(tapGain_[c][i]);
        }
        for (auto& g : tapGain_[c]) g /= sum;
        for (std::size_t i = 0; i < 4; ++i)
            allpassMs_[c][i] = (1 + 11 * static_cast<double>(i) / 3) * (.9 + .2 * random(seed));
    }
}
void Processor::Delay::prepare(double samples) {
    auto n = std::bit_ceil(static_cast<std::size_t>(std::ceil(samples)) + 8);
    data.resize(n);
    mask = n - 1;
    reset();
}
void Processor::Delay::reset() SUPPLE_RT {
    std::fill(data.begin(), data.end(), 0);
    head = 0;
}
double Processor::Delay::read(double samples) const SUPPLE_RT {
    // Hermite polynomial at head-delay, four causal neighbours (delay >= 2).
    const auto whole = static_cast<std::size_t>(samples);
    const double t = samples - static_cast<double>(whole);
    const auto p = (head - whole) & mask;
    const double a = data[(p + 1) & mask], b = data[p], c = data[(p - 1) & mask],
                 d = data[(p - 2) & mask];
    return b + .5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}
double Processor::Delay::modulated(
    double samples, const std::array<std::array<double, 12>, 257>& kernel) const SUPPLE_RT {
    const auto whole = static_cast<std::size_t>(samples);
    const auto position = (head - whole) & mask;
    if (samples == static_cast<double>(whole)) return data[position];
    const double phase = (samples - static_cast<double>(whole)) * 256;
    const auto row = static_cast<std::size_t>(phase);
    const double t = phase - static_cast<double>(row);
    double y = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        const double coefficient = kernel[row][i] + t * (kernel[row + 1][i] - kernel[row][i]);
        y += coefficient * data[(position + 5 - i) & mask];
    }
    return y;
}
Status Processor::prepare(double fs, std::uint32_t frames, int channels) {
    if (!std::isfinite(fs) || fs < 1 || fs > 768000 || frames == 0 || channels < 1 || channels > 2)
        return Error{"reverb.prepare.invalid", "Unsupported processing configuration",
                     "/sample_rate",
                     "Use a rate from 1 to 768000 Hz, positive maxFrames, and mono or stereo."};
    try {
        // Original Lagrange basis, twelve centred taps; adjacent polyphases
        // blend continuously. All construction is off the audio thread.
        for (std::size_t row = 0; row < interpolation_.size(); ++row) {
            const double t = static_cast<double>(row) / 256;
            double sum = 0;
            for (std::size_t i = 0; i < 12; ++i) {
                const double node = static_cast<double>(i) - 5;
                double coefficient = 1;
                for (std::size_t j = 0; j < 12; ++j)
                    if (i != j) {
                        const double other = static_cast<double>(j) - 5;
                        coefficient *= (t - other) / (node - other);
                    }
                interpolation_[row][i] = coefficient;
                sum += coefficient;
            }
            for (auto& coefficient : interpolation_[row]) coefficient /= sum;
        }
        fs_ = fs;
        channels_ = channels;
        glideAlpha_ = 1 - std::exp(-1 / (.05 * fs));
        preAlpha_ = glideAlpha_;
        freezeStep_ = 1 / (.01 * fs);
        for (auto& l : lines_) l.prepare(fs * (.300 + .001));
        for (std::size_t c = 0; c < 2; ++c) {
            early_[c].prepare(fs * .250);
            predelay_[c].prepare(fs * .501);
            for (auto& a : diffusers_[c]) a.prepare(fs * .040);
        }
        reset();
        return okStatus();
    } catch (const std::exception&) {
        return Error{"reverb.prepare.failed", "Could not allocate delay storage", "/max_frames",
                     "Reduce memory pressure and reactivate."};
    }
}
void Processor::reset() SUPPLE_RT {
    current_ = target_;
    remaining_ = {};
    moving_ = 0;
    step_ = {};
    started_ = false;
    dirty_ = coefficientsDirty_ = true;
    clock_ = 0;
    freeze_ = target_[kFreeze] > .5 ? 1 : 0;
    held_ = freeze_ == 1;
    peakIn_ = peakOut_ = 0;
    for (auto& l : lines_) l.reset();
    for (auto& channel : diffusers_)
        for (auto& l : channel) l.reset();
    for (auto& l : early_) l.reset();
    for (auto& l : predelay_) l.reset();
    low_ = {};
    high_ = {};
    cuts_ = {};
    scale_ = std::exp2((target_[kSize] - 50) * .03);
    std::uint32_t seed = 0x53504143;
    for (std::size_t i = 0; i < 16; ++i) {
        const double length = std::max(7., std::round(baseMs_[i] * scale_ * fs_ * .001));
        lengths_[i] = {length, length};
        lengthTarget_[i] = length;
        heldLength_[i] = static_cast<std::size_t>(lengths_[i].value);
        const double rate = .1 + 1.1 * (static_cast<double>(i) + .7 * random(seed)) / 16;
        const double phase = 2 * pi * random(seed), angle = 2 * pi * rate / fs_;
        oscSin_[i] = std::sin(phase);
        oscCos_[i] = std::cos(phase);
        oscStepSin_[i] = std::sin(angle);
        oscStepCos_[i] = std::cos(angle);
    }
    pre_ = {target_[kPredelay] * .001 * fs_, target_[kPredelay] * .001 * fs_};
    update();
}
void Processor::setParam(std::size_t i, double physical) SUPPLE_RT {
    if (i >= kNumParams) return;
    const double v = params::clampPhysical(table()[i], physical);
    if (v == target_[i] && started_) return;
    target_[i] = v;
    if ((kFilterControls & (1u << i)) || i == kFreeze) coefficientsDirty_ = true;
    if (!started_) {
        current_[i] = v;
        dirty_ = true;
        return;
    }
    if (i == kFreeze) {
        dirty_ = true;
        return;
    }
    const auto n = std::max(1u, static_cast<std::uint32_t>(std::lround(.020 * fs_)));
    remaining_[i] = n;
    step_[i] = (v - current_[i]) / n;
    if (i == kOutput) {
        outputTarget_ = std::pow(10., v / 20);
        outputStep_ = (outputTarget_ - outputGain_) / n;
    }
    moving_ |= 1u << i;
    dirty_ = true;
}
void Processor::Glide::tick(double target, double alpha) SUPPLE_RT {
    stage += (target - stage) * alpha;
    value += (stage - value) * alpha;
    if (std::abs(target - value) < 1e-8 && std::abs(target - stage) < 1e-8) value = stage = target;
}
void Processor::Shelf::design(double f, double ratio, double fs, bool high) SUPPLE_RT {
    ratio = std::clamp(ratio, 1e-120, 1e120);
    flat = ratio == 1;
    if (flat) {
        s1 = s2 = 0;
        return;
    }
    // Butterworth analog pole/zero pairs, bilinear transform. Geometric
    // placement makes f the midpoint and preserves both endpoint gains.
    const double root = std::sqrt(std::sqrt(ratio)), g = std::tan(pi * std::min(f, .45 * fs) / fs);
    const double pole = high ? g * root : g / root, zero = high ? g / root : g * root;
    const double level = high ? ratio : 1, k = std::numbers::sqrt2;
    const double denominator = 1 + k * pole + pole * pole;
    b0 = level * (1 + k * zero + zero * zero) / denominator;
    b1 = 2 * level * (zero * zero - 1) / denominator;
    b2 = level * (1 - k * zero + zero * zero) / denominator;
    a1 = 2 * (pole * pole - 1) / denominator;
    a2 = (1 - k * pole + pole * pole) / denominator;
}
double Processor::Shelf::run(double x) SUPPLE_RT {
    if (flat) return x;
    const double y = b0 * x + s1;
    s1 = flush(b1 * x - a1 * y + s2);
    s2 = flush(b2 * x - a2 * y);
    return y;
}
void Processor::Cut::design(double f, double fs) SUPPLE_RT {
    const double g = std::tan(pi * std::min(f, .45 * fs) / fs), k = std::numbers::sqrt2;
    a1 = 1 / (1 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}
double Processor::Cut::run(double x, bool high) SUPPLE_RT {
    const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
    s1 = flush(2 * v1 - s1);
    s2 = flush(2 * v2 - s2);
    return high ? x - std::numbers::sqrt2 * v1 - v2 : v2;
}
void Processor::update() SUPPLE_RT {
    if (!started_) {
        const double samples = current_[kPredelay] * .001 * fs_;
        pre_ = {samples, samples};
    }
    if (coefficientsDirty_ || !started_) {
        if (!held_) scale_ = std::exp2((current_[kSize] - 50) * .03);
        for (std::size_t c = 0; c < 2; ++c) {
            cuts_[c][0].design(current_[kLowCut], fs_);
            cuts_[c][1].design(current_[kHighCut], fs_);
            for (std::size_t j = 0; j < 12; ++j)
                tapLength_[c][j] = std::max(2., std::round(tapMs_[c][j] * scale_ * fs_ * .001));
            for (std::size_t j = 0; j < 4; ++j)
                allpassLength_[c][j] =
                    std::max(2., std::round(allpassMs_[c][j] * scale_ * fs_ * .001));
        }
        if (!started_)
            for (std::size_t c = 0; c < 2; ++c) {
                for (std::size_t j = 0; j < 12; ++j)
                    tapRead_[c][j] = {tapLength_[c][j], tapLength_[c][j]};
                for (std::size_t j = 0; j < 4; ++j)
                    allpassRead_[c][j] = {allpassLength_[c][j], allpassLength_[c][j]};
            }
        for (std::size_t i = 0; i < 16; ++i) {
            lengthTarget_[i] = held_ ? static_cast<double>(heldLength_[i])
                                     : std::max(7., std::round(baseMs_[i] * scale_ * fs_ * .001));
            if (!started_) {
                lengths_[i] = {lengthTarget_[i], lengthTarget_[i]};
                heldLength_[i] = static_cast<std::size_t>(lengths_[i].value);
            }
            const double seconds = lengths_[i].value / fs_, t = current_[kDecay] * .001;
            const double exponent = -3 * seconds / t * (1 - freeze_);
            gain_[i] = std::pow(10., exponent);
            low_[i].design(current_[kCrossover],
                           std::pow(10., exponent * (1 / current_[kLowDecay] - 1)), fs_, false);
            high_[i].design(current_[kDampingFreq],
                            std::pow(10., exponent * (1 / current_[kHighDecay] - 1)), fs_, true);
        }
        coefficientsDirty_ = false;
    }
    allpassGain_ = .7 * current_[kDiffusion] * .01;
    earlyGain_ = current_[kEarly] <= -60 ? 0 : std::pow(10., current_[kEarly] / 20);
    if (!started_) outputGain_ = outputTarget_ = std::pow(10., current_[kOutput] / 20);
    modDepth_ = .0005 * fs_ * current_[kModulation] * .01;
    double longestAllpass = 0;
    for (const auto& channel : allpassLength_)
        for (double length : channel) longestAllpass = std::max(longestAllpass, length);
    const double shortest =
        current_[kDecay] * .001 * std::min({1., current_[kLowDecay], current_[kHighDecay]});
    allpassGain_ =
        std::min(allpassGain_, std::pow(10., -3 * longestAllpass / (fs_ * .5 * shortest)));
    dirty_ = false;
}
void Processor::tick(const std::array<double, 2>& x, std::array<double, 2>& output) SUPPLE_RT {
    if (moving_ & kFilterControls) coefficientsDirty_ = true;
    if (moving_) {
        for (std::size_t i = 0; i < kNumParams; ++i)
            if (remaining_[i]) {
                current_[i] += step_[i];
                if (i == kOutput) outputGain_ += outputStep_;
                if (--remaining_[i] == 0) {
                    current_[i] = target_[i];
                    if (i == kOutput) outputGain_ = outputTarget_;
                    moving_ &= ~(1u << i);
                }
            }
        dirty_ = true;
    }
    const double wanted = target_[kFreeze] > .5 ? 1 : 0;
    if (wanted && !held_) {
        held_ = true;
        for (std::size_t i = 0; i < 16; ++i)
            heldLength_[i] = static_cast<std::size_t>(std::lround(lengths_[i].value));
    }
    if (freeze_ != wanted) {
        freeze_ += std::clamp(wanted - freeze_, -freezeStep_, freezeStep_);
        dirty_ = coefficientsDirty_ = true;
    }
    if (!wanted && freeze_ == 0) held_ = false;
    bool lengthMoving = false;
    for (std::size_t i = 0; i < 16; ++i) {
        const double target = lengthTarget_[i];
        if (lengths_[i].value != target || lengths_[i].stage != target) {
            lengths_[i].tick(target, glideAlpha_);
            lengthMoving = true;
        }
    }
    if (lengthMoving) dirty_ = coefficientsDirty_ = true;
    if ((clock_ & 15) == 0 && dirty_) update();
    pre_.tick(current_[kPredelay] * .001 * fs_, preAlpha_);
    std::array<double, 2> diffuse{}, early{};
    for (std::size_t c = 0; c < 2; ++c) {
        double v = cuts_[c][1].run(cuts_[c][0].run(x[c], true), false);
        // Writing before reading makes predelay 0 exactly zero delay.
        predelay_[c].write(v);
        const double delay = pre_.value;
        v = delay < 1 ? ((1 - delay) * v + delay * predelay_[c].integer(2))
                      : predelay_[c].read(delay + 1);
        v *= 1 - freeze_;
        early_[c].write(v);
        for (std::size_t j = 0; j < 4; ++j) {
            auto& line = diffusers_[c][j];
            auto& read = allpassRead_[c][j];
            const double target = allpassLength_[c][j];
            if (read.value != target || read.stage != target) read.tick(target, glideAlpha_);
            const double d = read.value == target ? line.integer(static_cast<std::size_t>(target))
                                                  : line.read(read.value);
            const double y = d - allpassGain_ * v;
            line.write(v + allpassGain_ * y);
            v = y;
        }
        diffuse[c] = v * (1 - freeze_);
    }
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t j = 0; j < 12; ++j) {
            auto& read = tapRead_[c][j];
            const double target = tapLength_[c][j];
            if (read.value != target || read.stage != target) read.tick(target, glideAlpha_);
            const double delayed = read.value == target
                                       ? early_[c].integer(static_cast<std::size_t>(target) + 1)
                                       : early_[c].read(read.value + 1);
            const double tap = tapGain_[c][j] * delayed;
            const double pan = (j + c) % 2 ? .2 : .8;
            early[0] += pan * tap;
            early[1] += (1 - pan) * tap;
        }
    std::array<double, 16> feedback{};
    std::array<double, 2> late{};
    for (std::size_t i = 0; i < 16; ++i) {
        const double s = oscSin_[i], co = oscCos_[i];
        oscSin_[i] = s * oscStepCos_[i] + co * oscStepSin_[i];
        oscCos_[i] = co * oscStepCos_[i] - s * oscStepSin_[i];
        const double delay = lengths_[i].value + modDepth_ * s * (1 - freeze_);
        const double v = freeze_ == 1 ? lines_[i].integer(heldLength_[i])
                                      : lines_[i].modulated(std::max(7., delay), interpolation_);
        late[0] += outL[i] * v;
        late[1] += outR[i] * v;
        const double normal = high_[i].run(low_[i].run(v)) * gain_[i];
        // At freeze=1 bypass recursive rounding and reset stale damping memory.
        feedback[i] = freeze_ == 1 ? v : normal;
    }
    // Orthogonal Walsh-Hadamard mixing, 16 log2(16) additions.
    for (std::size_t stride = 1; stride < 16; stride *= 2)
        for (std::size_t base = 0; base < 16; base += 2 * stride)
            for (std::size_t j = 0; j < stride; ++j) {
                double a = feedback[base + j], b = feedback[base + j + stride];
                feedback[base + j] = a + b;
                feedback[base + j + stride] = a - b;
            }
    for (std::size_t i = 0; i < 16; ++i)
        lines_[i].write(.25 * feedback[i] + .25 * (inL[i] * diffuse[0] + inR[i] * diffuse[1]));
    const double left = late[0] / std::sqrt(8.) + earlyGain_ * early[0],
                 right = late[1] / std::sqrt(8.) + earlyGain_ * early[1];
    const double mid = .5 * (left + right), side = .5 * (left - right) * current_[kWidth] * .01;
    const double mix = current_[kMix] * .01;
    output[0] = outputGain_ * ((1 - mix) * x[0] + mix * (mid + side));
    output[1] = outputGain_ * ((1 - mix) * x[1] + mix * (mid - side));
    if (channels_ == 1) output[0] = outputGain_ * ((1 - mix) * x[0] + mix * mid);
    if (telemetryEnabled_) {
        peakIn_ = std::max({peakIn_, std::abs(x[0]), std::abs(x[1])});
        peakOut_ = std::max({peakOut_, std::abs(output[0]), std::abs(output[1])});
    }
    ++clock_;
}
Processor::Telemetry Processor::takeTelemetry() SUPPLE_RT {
    Telemetry t{20 * std::log10(std::max(1e-6, peakIn_)),
                20 * std::log10(std::max(1e-6, peakOut_))};
    peakIn_ = peakOut_ = 0;
    return t;
}
}  // namespace supple::reverb
