// Control-rate modulators: envelopes, LFOs, step sequencers, smooth random.
// Each advances by a number of base-rate samples per control tick and
// reports its value at the end of the tick. RT-safe, allocation-free,
// deterministic (randomness from a seeded Rng).
#pragma once

#include <array>
#include <cmath>

#include "dsp.h"
#include "orrery/params.h"
#include "supple/base/rng.h"

namespace supple::orrery {

// DAHDSR envelope (synth.md §4.6). Times in samples, curves -1..1.
struct EnvParams {
    double delay, attack, hold, decay, sustain, release;
    double attackCurve, decayCurve, releaseCurve;
};

class Envelope {
  public:
    enum class Stage : std::uint8_t { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

    // (Re)start from the current level (no click on retrigger).
    void gate(const EnvParams&) SUPPLE_RT {
        start_ = level_;
        stage_ = Stage::Delay;
        x_ = 0.0;
    }
    void release() SUPPLE_RT {
        if (stage_ == Stage::Idle || stage_ == Stage::Release) return;
        start_ = level_;
        stage_ = Stage::Release;
        x_ = 0.0;
    }
    void kill() SUPPLE_RT {
        stage_ = Stage::Idle;
        level_ = 0.0;
    }
    Stage stage() const { return stage_; }
    bool idle() const { return stage_ == Stage::Idle; }
    double level() const { return level_; }

    // Advances by `dt` samples (stage changes inside the step are honoured;
    // leftover time carries into the next stage).
    double advance(double dt, const EnvParams& p) SUPPLE_RT {
        for (int guard = 0; guard < 8 && dt > 0.0; ++guard) {
            switch (stage_) {
                case Stage::Idle:
                    level_ = 0.0;
                    return level_;
                case Stage::Sustain:
                    level_ = p.sustain;
                    return level_;
                case Stage::Delay:
                    dt = timed(dt, p.delay, Stage::Attack, [&](double) { return start_; });
                    break;
                case Stage::Attack:
                    dt = timed(dt, p.attack, Stage::Hold, [&](double x) {
                        return start_ + (1.0 - start_) * dsp::shapeRise(x, p.attackCurve);
                    });
                    if (stage_ == Stage::Hold) start_ = 1.0;
                    break;
                case Stage::Hold:
                    dt = timed(dt, p.hold, Stage::Decay, [&](double) { return 1.0; });
                    break;
                case Stage::Decay:
                    dt = timed(dt, p.decay, Stage::Sustain, [&](double x) {
                        return 1.0 + (p.sustain - 1.0) * dsp::shapeFall(x, p.decayCurve);
                    });
                    break;
                case Stage::Release:
                    dt = timed(dt, p.release, Stage::Idle, [&](double x) {
                        return start_ * (1.0 - dsp::shapeFall(x, p.releaseCurve));
                    });
                    break;
            }
        }
        return level_;
    }

  private:
    // Runs the current stage for up to dt samples; returns the leftover.
    template <class F>
    double timed(double dt, double duration, Stage next, F&& curve) SUPPLE_RT {
        if (duration <= 0.5) {
            x_ = 1.0;
            level_ = curve(1.0);
            enter(next);
            return dt;
        }
        const double remaining = (1.0 - x_) * duration;
        if (dt < remaining) {
            x_ += dt / duration;
            level_ = curve(x_);
            return 0.0;
        }
        level_ = curve(1.0);
        enter(next);
        return dt - remaining;
    }
    void enter(Stage s) SUPPLE_RT {
        stage_ = s;
        x_ = 0.0;
        if (s == Stage::Idle) level_ = 0.0;
    }

    Stage stage_ = Stage::Idle;
    double level_ = 0.0, start_ = 0.0, x_ = 0.0;
};

// LFO (synth.md §4.7). Phase in cycles.
struct LfoParams {
    LfoWave wave;
    double hz;     // resolved rate (sync already applied)
    double phase;  // start phase in cycles
    double fade;   // samples
    bool unipolar;
};

class Lfo {
  public:
    void seed(std::uint64_t s) SUPPLE_RT { rng_.reseed(s); }
    void restart(const LfoParams& p) SUPPLE_RT {
        phase_ = dsp::wrap01(p.phase);
        age_ = 0.0;
        hold_ = rng_.uniform(-1.0, 1.0);
        prev_ = hold_;
        next_ = rng_.uniform(-1.0, 1.0);
    }
    double advance(double dt, double sampleRate, const LfoParams& p) SUPPLE_RT {
        phase_ += p.hz * dt / sampleRate;
        age_ += dt;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            prev_ = next_;
            hold_ = next_;
            next_ = rng_.uniform(-1.0, 1.0);
        }
        double v = 0.0;
        switch (p.wave) {
            case LfoWave::Sine:
                v = std::sin(dsp::kTwoPi * phase_);
                break;
            case LfoWave::Triangle:
                v = 1.0 - 4.0 * std::fabs(phase_ - 0.5);
                break;
            case LfoWave::SawUp:
                v = 2.0 * phase_ - 1.0;
                break;
            case LfoWave::SawDown:
                v = 1.0 - 2.0 * phase_;
                break;
            case LfoWave::Square:
                v = phase_ < 0.5 ? 1.0 : -1.0;
                break;
            case LfoWave::SampleHold:
                v = hold_;
                break;
            case LfoWave::SmoothRandom: {
                const double s = 0.5 - 0.5 * std::cos(dsp::kPi * phase_);
                v = prev_ + (next_ - prev_) * s;
                break;
            }
        }
        if (p.fade > 0.0 && age_ < p.fade) v *= age_ / p.fade;
        return p.unipolar ? 0.5 * (v + 1.0) : v;
    }

  private:
    Rng rng_;
    double phase_ = 0.0, age_ = 0.0, hold_ = 0.0, prev_ = 0.0, next_ = 0.0;
};

// Step modulator (synth.md §4.8): bipolar values, optional glide.
class StepSeq {
  public:
    void restart() SUPPLE_RT {
        pos_ = 0.0;
        value_ = kUnset;
    }
    // stepSamples: length of one step; values in -1..1 (length entries used).
    double advance(double dt, double stepSamples, const std::array<float, kMaxSteps>& values,
                   int length, double glide) SUPPLE_RT {
        length = std::clamp(length, 1, kMaxSteps);
        pos_ += dt / std::max(1.0, stepSamples);
        pos_ = std::fmod(pos_, static_cast<double>(length));
        const auto target = static_cast<double>(values[static_cast<std::size_t>(pos_)]);
        if (value_ == kUnset || glide <= 0.0) {
            value_ = target;
        } else {
            // One-pole toward the step value; glide 1 = a full step's time constant.
            const double tau = glide * stepSamples * 0.25;
            value_ += (target - value_) * (1.0 - std::exp(-dt / std::max(1.0, tau)));
        }
        return value_;
    }

  private:
    static constexpr double kUnset = 1e9;
    double pos_ = 0.0, value_ = kUnset;
};

}  // namespace supple::orrery
