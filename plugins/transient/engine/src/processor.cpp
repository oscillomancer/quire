// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "transient/processor.h"
namespace supple::transient {
namespace {
double db(double x) SUPPLE_RT { return 20 * std::log10(std::max(x, 1e-300)); }
double flush(double x) SUPPLE_RT { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace
Processor::Processor() : target_(defaults()), current_(target_) {}
Status Processor::prepare(double sr, std::uint32_t maxFrames, int channels) {
    if (!std::isfinite(sr) || sr <= 0 || sr > 768000 || maxFrames == 0 ||
        (channels != 1 && channels != 2))
        return Error{"invalid-prepare", "Invalid sample rate, block size or channel count.",
                     "prepare",
                     "Use a positive rate through 768 kHz, nonzero frames and mono/stereo."};
    sampleRate_ = sr;
    channels_ = channels;
    minHold_ = std::max(1u, static_cast<std::uint32_t>(std::lround(sr * .001)));
    maxHold_ = std::max(minHold_, static_cast<std::uint32_t>(std::lround(sr * .05)));
    for (auto& os : oversamplers_) os.prepare(2, maxFrames);
    try {
        peaks_.resize(maxHold_ + 2);
        for (auto& dry : dry_) dry.resize(oversamplers_[0].latency() + 1);
    } catch (...) {
        return Error{"allocation", "Cannot prepare detector and clip storage.", "prepare",
                     "Reduce sample rate or free memory."};
    }
    smooth_ = std::exp(-1 / (sr * .02));
    gainAlpha_ = std::exp(-1 / (sr * .001));
    ready_ = true;
    reset();
    return {};
}
void Processor::reset() SUPPLE_RT {
    current_ = target_;
    for (auto& os : oversamplers_) os.reset();
    for (auto& dry : dry_) std::fill(dry.begin(), dry.end(), 0.);
    read_ = write_ = dryHead_ = 0;
    clock_ = 0;
    fast_ = slow_ = gainDb_ = reference_ = lastSpeed_ = inputPeak_ = outputPeak_ = gainSum_ = 0;
    meterSamples_ = 0;
    started_ = detectorStarted_ = havePeak_ = false;
    previous_ = previous2_ = 0;
    lastPeak_ = 0;
    hold_ = minHold_;
}
void Processor::setParam(std::size_t i, double v) SUPPLE_RT {
    if (i >= kInputLevel || !std::isfinite(v)) return;
    const auto& p = table()[i];
    v = std::clamp(v, p.min, p.max);
    if (p.type == params::Type::Bool) v = v > .5 ? 1 : 0;
    target_[i] = v;
    if (!started_) current_[i] = v;
}
double Processor::detect(double x) SUPPLE_RT {
    // A cycle-aware peak hold (1..50 ms) removes carrier zero crossings without
    // adding audio latency; a bounded monotonic deque keeps it sample-invariant.
    if (previous_ >= previous2_ && previous_ > x) {
        if (havePeak_)
            hold_ = static_cast<std::uint32_t>(std::clamp(clock_ - lastPeak_ + 1,
                                                          static_cast<std::uint64_t>(minHold_),
                                                          static_cast<std::uint64_t>(maxHold_)));
        lastPeak_ = clock_;
        havePeak_ = true;
    }
    previous2_ = previous_;
    previous_ = x;
    const auto size = peaks_.size();
    while (read_ != write_ && clock_ - peaks_[read_].time >= hold_) read_ = (read_ + 1) % size;
    while (read_ != write_) {
        const auto back = (write_ + size - 1) % size;
        if (peaks_[back].value > x) break;
        write_ = back;
    }
    peaks_[write_] = {clock_++, x};
    write_ = (write_ + 1) % size;
    return peaks_[read_].value;
}
void Processor::tick(const std::array<double, 2>& x, std::array<double, 2>& y) SUPPLE_RT {
    for (std::size_t i = 0; i < kInputLevel; ++i) {
        if (i == kClip || current_[i] == target_[i]) continue;
        if (i == kSpeed)
            current_[i] =
                std::exp(smooth_ * std::log(current_[i]) + (1 - smooth_) * std::log(target_[i]));
        else
            current_[i] = smooth_ * current_[i] + (1 - smooth_) * target_[i];
        if (std::abs(current_[i] - target_[i]) < 1e-12) current_[i] = target_[i];
    }
    const double speed = current_[kSpeed];
    if (speed != lastSpeed_) {
        fastAttack_ = std::exp(-1 / (sampleRate_ * speed * .00001));
        fastRelease_ = std::exp(-1 / (sampleRate_ * speed * .001));
        slowRelease_ = std::exp(-1 / (sampleRate_ * speed * .004));
        lastSpeed_ = speed;
    }
    const double rect = std::max(std::abs(x[0]), std::abs(x[1]));
    const double peak = detect(rect);
    if (peak > 0 && !detectorStarted_) {
        reference_ = db(peak);
        fast_ = slow_ = reference_ - 60;
        detectorStarted_ = true;
    }
    if (detectorStarted_) {
        reference_ = std::max(reference_, db(peak));
        const double level = std::max(db(peak), reference_ - 120);
        const double fa = level > fast_ ? fastAttack_ : fastRelease_,
                     sa = level > slow_ ? fastRelease_ : slowRelease_;
        fast_ = flush(fa * fast_ + (1 - fa) * level);
        slow_ = flush(sa * slow_ + (1 - sa) * level);
        const double d = (fast_ - slow_) * current_[kSensitivity] / 500.;
        const double g = .12 * (current_[kAttack] * std::clamp(d, 0., 1.) +
                                current_[kSustain] * std::clamp(-d, 0., 1.));
        gainDb_ = flush(gainAlpha_ * gainDb_ + (1 - gainAlpha_) * g);
    }
    const double gain = std::pow(10., gainDb_ / 20), out = std::pow(10., current_[kOutput] / 20),
                 mix = current_[kMix] / 100;
    const bool clip = target_[kClip] > .5;
    for (std::size_t c = 0; c < 2; ++c) {
        double wet = x[c] * gain, dry = x[c];
        if (clip) {
            dry_[c][dryHead_] = x[c];
            dry = dry_[c][(dryHead_ + 1) % dry_[c].size()];
            std::array<double, 2> hi{};
            oversamplers_[c].upsample(wet, hi);
            for (auto& sample : hi) sample = std::tanh(sample);
            wet = oversamplers_[c].downsample(hi);
        }
        // Preserve the transparent default, including signed zeros.
        y[c] = out * (mix == 1 ? wet : mix == 0 ? dry : dry + mix * (wet - dry));
        inputPeak_ = std::max(inputPeak_, std::abs(x[c]));
        outputPeak_ = std::max(outputPeak_, std::abs(y[c]));
    }
    if (clip) dryHead_ = (dryHead_ + 1) % dry_[0].size();
    gainSum_ += gainDb_;
    ++meterSamples_;
}
Processor::Telemetry Processor::takeTelemetry() SUPPLE_RT {
    Telemetry t{std::max(-120., db(inputPeak_)), std::max(-120., db(outputPeak_)),
                meterSamples_ ? gainSum_ / static_cast<double>(meterSamples_) : 0};
    inputPeak_ = outputPeak_ = gainSum_ = 0;
    meterSamples_ = 0;
    return t;
}
}  // namespace supple::transient
