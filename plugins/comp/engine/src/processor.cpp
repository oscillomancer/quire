#include "comp/processor.h"
#include <exception>
#include <limits>
#include <numbers>

namespace supple::comp {
namespace {
double db(double x) SUPPLE_RT { return 20 * std::log10(std::max(x, 1e-6)); }
double gain(double x) SUPPLE_RT { return std::pow(10., x / 20); }
double flush(double x) SUPPLE_RT { return std::abs(x) < 1e-30 ? 0 : x; }
}  // namespace
Processor::Processor() : target_(defaults()), current_(target_) {}
Status Processor::prepare(double sr, std::uint32_t maxFrames, int channels) {
    if (!std::isfinite(sr) || sr <= 0 || sr > 768000 || maxFrames == 0 || channels < 1 ||
        channels > 2)
        return Error{"comp.prepare.invalid", "Unsupported processing configuration", "/sample_rate",
                     "Use a positive sample rate up to 768000 Hz, positive maxFrames, and one or "
                     "two channels."};
    try {
        sampleRate_ = sr;
        channels_ = std::clamp(channels, 1, 2);
        smooth_ = 1 - std::exp(-1 / (.020 * sampleRate_));
        switch_ = 1 / (.010 * sampleRate_);
        const auto capacity = static_cast<std::size_t>(std::ceil(.005 * sampleRate_)) + 18;
        for (std::size_t c = 0; c < 2; ++c) {
            delay_[c].resize(capacity);
            keyDelay_[c].resize(capacity);
            average_[c].resize(capacity);
            minimum_[c].queue.resize(2 * capacity + 2);
            interpolators_[c].prepare(4);
        }
        reset();
        return okStatus();
    } catch (const std::exception&) {
        return Error{"comp.prepare.failed", "Could not prepare dynamics storage", "/max_frames",
                     "Reduce memory pressure and activate again."};
    }
}
void Processor::reset() SUPPLE_RT {
    current_ = target_;
    inverseRatio_ = target_[kRatio] >= 100 ? 0 : 1 / target_[kRatio];
    derived_.fill(std::numeric_limits<double>::quiet_NaN());
    peaks_ = {};
    started_ = false;
    hp_ = {};
    lp_ = {};
    rms_ = {};
    reduction_ = {};
    feedback_ = {};
    held_ = {};
    limiterGain_ = {1, 1};
    averageSum_ = {};
    head_ = averageHead_ = averageSize_ = 0;
    clock_ = 0;
    meters_ = {};
    for (std::size_t c = 0; c < 2; ++c) {
        std::fill(delay_[c].begin(), delay_[c].end(), 0);
        std::fill(keyDelay_[c].begin(), keyDelay_[c].end(), 0);
        std::fill(average_[c].begin(), average_[c].end(), 1);
        minimum_[c].reset();
        interpolators_[c].reset();
    }
}
void Processor::setParam(std::size_t i, double v) SUPPLE_RT {
    if (i >= kNumParams || i == kGr) return;
    target_[i] = params::clampPhysical(table()[i], v);
    if (!started_) {
        current_[i] = target_[i];
        if (i == kRatio) inverseRatio_ = target_[i] >= 100 ? 0 : 1 / target_[i];
    }
}
double Processor::Filter::run(double x, double g, bool high) SUPPLE_RT {
    constexpr double k = 1.4142135623730951;
    const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const double v3 = x - b, v1 = a1 * a + a2 * v3, v2 = b + a2 * a + a3 * v3;
    a = flush(2 * v1 - a);
    b = flush(2 * v2 - b);
    return high ? x - k * v1 - v2 : v2;
}
double Processor::Minimum::push(std::uint64_t t, double v, std::uint32_t width) SUPPLE_RT {
    const auto size = queue.size();
    while (read != write && t - queue[read % size].time > width) ++read;
    while (read != write && queue[(write - 1) % size].value >= v) --write;
    queue[write % size] = {t, v};
    ++write;
    return queue[read % size].value;
}
void Processor::tick(const std::array<double, 2>& x, const std::array<double, 2>& key,
                     std::array<double, 2>& y) SUPPLE_RT {
    for (std::size_t i = 0; i < kGr; ++i) {
        if (current_[i] == target_[i]) continue;
        const auto& p = table()[i];
        if (i == kMode || i == kLookahead || i == kTruePeak) {
            current_[i] = target_[i];
            continue;
        }
        if (p.type != params::Type::Float) {
            current_[i] += std::clamp(target_[i] - current_[i], -switch_, switch_);
        } else if (p.curve.kind == params::CurveKind::Log) {
            current_[i] = std::exp(std::log(current_[i]) +
                                   smooth_ * (std::log(target_[i]) - std::log(current_[i])));
        } else
            current_[i] += smooth_ * (target_[i] - current_[i]);
        if (std::abs(current_[i] - target_[i]) < 1e-12) current_[i] = target_[i];
    }
    const double ratioTarget = target_[kRatio] >= 100 ? 0 : 1 / target_[kRatio];
    inverseRatio_ += smooth_ * (ratioTarget - inverseRatio_);
    if (std::abs(inverseRatio_ - ratioTarget) < 1e-12) inverseRatio_ = ratioTarget;
    const auto& v = current_;
    const std::uint32_t delay = latency();
    const bool limiter = v[kMode] > .5, tp = v[kTruePeak] > .5;
    // Interpolated detection is delayed by 16 samples; only the actual
    // lookahead belongs in the gain ramp, so it finishes at the delayed peak.
    const auto averageSize = static_cast<std::size_t>(delay - (limiter && tp ? 16 : 0)) + 1;
    const auto capacity = delay_[0].size();
    if (averageSize_ != averageSize) {
        averageSize_ = averageSize;
        averageHead_ = 0;
        averageSum_.fill(static_cast<double>(averageSize_));
        for (auto& samples : average_) std::fill(samples.begin(), samples.end(), 1);
    }
    const double link = channels_ == 1 ? 1 : v[kStereoLink] / 100, wet = v[kMix] / 100;
    const double makeup =
        v[kMakeup] - (limiter ? 0
                              : v[kAutoMakeup] * reductionFromSlope(-18, v[kThreshold],
                                                                    inverseRatio_ - 1, v[kKnee]));
    if (v[kOutput] != derived_[kOutput]) outGain_ = gain(v[kOutput]);
    if (makeup != derived_[kMakeup]) wetGain_ = gain(makeup);
    if (v[kCeiling] != derived_[kCeiling]) ceilingGain_ = gain(v[kCeiling]);
    if (v[kScHighpass] != derived_[kScHighpass])
        hpG_ =
            std::tan(std::numbers::pi * std::min(v[kScHighpass], .45 * sampleRate_) / sampleRate_);
    if (v[kScLowpass] != derived_[kScLowpass])
        lpG_ =
            std::tan(std::numbers::pi * std::min(v[kScLowpass], .45 * sampleRate_) / sampleRate_);
    if (v[kRmsWindow] != derived_[kRmsWindow])
        rmsAlpha_ = std::exp(-1 / (v[kRmsWindow] * .001 * sampleRate_));
    if (v[kAttack] != derived_[kAttack])
        attackAlpha_ = std::exp(-1 / (v[kAttack] * .001 * sampleRate_));
    if (v[kRelease] != derived_[kRelease]) {
        releaseAlpha_ = std::exp(-1 / (v[kRelease] * .001 * sampleRate_));
        slowAlpha_ = std::exp(-1 / (5 * v[kRelease] * .001 * sampleRate_));
    }
    derived_ = v;
    derived_[kMakeup] = makeup;
    const double outGain = outGain_, wetGain = wetGain_;
    const double hpG = hpG_, lpG = lpG_, rmsAlpha = rmsAlpha_;
    const double hpOn = std::clamp((v[kScHighpass] - 10) / 10, 0., 1.);
    const double lpOn = std::clamp((20000 - v[kScLowpass]) / 1000, 0., 1.);
    std::array<double, 2> filtered{}, levels{}, limitTarget{};
    for (std::size_t c = 0; c < 2; ++c) {
        std::array<double, 3> signals{x[c], feedback_[c], key[c]}, f{}, level{};
        for (std::size_t s = 0; s < 3; ++s) {
            const double h = hpOn > 0 ? hp_[s][c].run(signals[s], hpG, true) : signals[s];
            const double a = signals[s] + hpOn * (h - signals[s]);
            const double l = lpOn > 0 ? lp_[s][c].run(a, lpG, false) : a;
            f[s] = a + lpOn * (l - a);
            rms_[s][c] = flush(rmsAlpha * rms_[s][c] + (1 - rmsAlpha) * f[s] * f[s]);
            level[s] = (1 - v[kDetector]) * std::abs(f[s]) +
                       v[kDetector] * (v[kDetector] > 0 ? std::sqrt(std::max(0., rms_[s][c])) : 0);
        }
        const double topology = limiter ? 0 : v[kTopology];
        filtered[c] =
            (1 - v[kSidechain]) * ((1 - topology) * f[0] + topology * f[1]) + v[kSidechain] * f[2];
        levels[c] =
            (1 - v[kSidechain]) * ((1 - v[kTopology]) * level[0] + v[kTopology] * level[1]) +
            v[kSidechain] * level[2];
        const double driven = (1 - v[kScListen]) * x[c] * outGain * ((1 - wet) + wet * wetGain) +
                              v[kScListen] * filtered[c];
        delay_[c][head_] = limiter ? driven : x[c];
        // Keep key history hot even while audition is off, so switching
        // audition uses the same source time as the latency-aligned dry path.
        keyDelay_[c][head_] = filtered[c];
        double peak = std::abs(driven);
        if (limiter && tp) {
            std::array<double, 4> phases{};
            interpolators_[c].process(static_cast<float>(driven), phases);
            peak = 0;
            for (double phase : phases) peak = std::max(peak, std::abs(phase));
        }
        const double ceiling = ceilingGain_;
        const double target = peak > ceiling ? ceiling / peak : 1;
        // With TP lookahead the extended hold protects each entire FIR support.
        const auto width = tp && v[kLookahead] > .5 ? 2 * delay : delay;
        const double minimum = minimum_[c].push(clock_, target, width);
        const auto slot = averageHead_ % averageSize;
        if (clock_ == 0) averageSum_[c] = static_cast<double>(averageSize);
        averageSum_[c] += minimum - average_[c][slot];
        average_[c][slot] = minimum;
        const double averaged = averageSum_[c] / static_cast<double>(averageSize);
        const double releaseAlpha = releaseAlpha_;
        limiterGain_[c] =
            std::min(averaged, releaseAlpha * limiterGain_[c] + (1 - releaseAlpha) * averaged);
        limitTarget[c] = limiterGain_[c];
    }
    const double linked = std::max(levels[0], levels[1]);
    for (std::size_t c = 0; c < 2; ++c) {
        const double level = link * linked + (1 - link) * levels[c];
        const double desired =
            reductionFromSlope(db(level), v[kThreshold], inverseRatio_ - 1, v[kKnee]);
        held_[c] = desired <= reduction_[c] && desired < -.1
                       ? std::min(held_[c] + 1 / sampleRate_, 1.)
                       : std::max(held_[c] - 1 / sampleRate_, 0.);
        const double attackAlpha = attackAlpha_, fast = releaseAlpha_, slow = slowAlpha_;
        const double weight = v[kAutoRelease] * held_[c];
        const double alpha =
            desired < reduction_[c] ? attackAlpha : (1 - weight) * fast + weight * slow;
        reduction_[c] = flush(alpha * reduction_[c] + (1 - alpha) * desired);
        const double dry = delay_[c][(head_ + capacity - delay) % capacity];
        const double compressed = dry * gain(reduction_[c]);
        feedback_[c] = compressed;
        if (limiter) {
            const double g =
                link * std::min(limitTarget[0], limitTarget[1]) + (1 - link) * limitTarget[c];
            y[c] = std::clamp(dry * g, -ceilingGain_, ceilingGain_);
            meters_.gr = std::min(meters_.gr, db(g));
        } else {
            y[c] = outGain * (wet * compressed * wetGain + (1 - wet) * dry);
            meters_.gr = std::min(meters_.gr, reduction_[c]);
        }
        if (!limiter) {
            const double listened = keyDelay_[c][(head_ + capacity - delay) % capacity];
            y[c] += v[kScListen] * (listened - y[c]);
        }
        // Auditioning a key still respects a limiter's final sample ceiling.
        if (limiter) y[c] = std::clamp(y[c], -ceilingGain_, ceilingGain_);
        peaks_[0] = std::max(peaks_[0], std::abs(x[c]));
        peaks_[1] = std::max(peaks_[1], std::abs(y[c]));
        peaks_[2] = std::max(peaks_[2], limiter ? std::abs(delay_[c][head_]) : level);
    }
    head_ = (head_ + 1) % capacity;
    ++averageHead_;
    ++clock_;
}
Processor::Telemetry Processor::takeTelemetry() SUPPLE_RT {
    Telemetry t{db(peaks_[0]), db(peaks_[1]), meters_.gr, db(peaks_[2])};
    meters_ = {};
    peaks_ = {};
    return t;
}
}  // namespace supple::comp
