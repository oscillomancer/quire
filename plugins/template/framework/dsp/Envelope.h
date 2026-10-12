// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// An ADSR envelope, per sample, with a curve per segment: 0 is a straight
// line, exactly; towards 1, the attack bows up (fast, then easing in) and
// the decay and release fall fast and tail off, as analog ones do. Each time
// is for the whole range, 0 to 1, so it sets a rate: an attack takes its
// time from silence. For linear segments, a decay to sustain 0.5 or a
// release from 0.5 takes half its time; curved segments take longer than
// that fraction, since they spend more of their time near the end.
//
//   env.setTimes(5, 200, 0.6, 400, sampleRate);   // ms, ms, level, ms
//   env.gate(true);  ...  env.gate(false);
//   y = osc * env.tick();   if (!env.active()) the voice is over
//
// A sustain level that moves while a note is held moves the note. A gate
// on while the envelope is still releasing starts the attack from where it
// is (no click); retrigger(), for legato, does the same while held.
#pragma once

#include <algorithm>
#include <cmath>

namespace supple::dsp {

class Adsr {
  public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setTimes(double attackMs, double decayMs, double sustain, double releaseMs, double sampleRate) {
        attack_ = segment(attackMs, sampleRate, attackCurve_);
        decay_ = segment(decayMs, sampleRate, curve_);
        release_ = segment(releaseMs, sampleRate, curve_);
        sustain_ = std::clamp(sustain, 0.0, 1.0);
        attackMs_ = attackMs;
        decayMs_ = decayMs;
        releaseMs_ = releaseMs;
        sampleRate_ = sampleRate;
    }
    // 0 linear .. 1 most curved; the attack has its own.
    void setCurves(double attackCurve, double decayReleaseCurve) {
        attackCurve_ = std::clamp(attackCurve, 0.0, 1.0);
        curve_ = std::clamp(decayReleaseCurve, 0.0, 1.0);
        setTimes(attackMs_, decayMs_, sustain_, releaseMs_, sampleRate_);
    }

    void gate(bool on) {
        if (on) {
            stage_ = Stage::Attack;
        } else if (stage_ != Stage::Idle) {
            stage_ = Stage::Release;
        }
    }
    // Back to the attack from the present level, for a legato note that
    // should still be heard to start; a held note stays where it is
    // otherwise.
    void retrigger() { stage_ = Stage::Attack; }
    // Silence, at once.
    void kill() {
        stage_ = Stage::Idle;
        level_ = 0.0;
    }

    double tick() {
        switch (stage_) {
            case Stage::Idle: return 0.0;
            case Stage::Attack:
                if (step(attack_, 1.0, true) >= 1.0) {
                    level_ = 1.0;
                    stage_ = Stage::Decay;
                }
                break;
            case Stage::Decay:
                if (step(decay_, sustain_, false) <= sustain_) {
                    level_ = sustain_;
                    stage_ = Stage::Sustain;
                }
                break;
            case Stage::Sustain: level_ = sustain_; break;
            case Stage::Release:
                if (step(release_, 0.0, false) <= 0.0) {
                    level_ = 0.0;
                    stage_ = Stage::Idle;
                }
                break;
        }
        return level_;
    }

    double level() const { return level_; }
    Stage stage() const { return stage_; }
    bool active() const { return stage_ != Stage::Idle; }
    // A note is still held (not releasing or over).
    bool held() const { return stage_ == Stage::Attack || stage_ == Stage::Decay || stage_ == Stage::Sustain; }

  private:
    // A segment is either a fixed step per sample (linear) or a one-pole
    // aimed past its end by `overshoot` (Nigel Redmon's form): the smaller
    // the overshoot, the stronger the curve. Either covers the range 0..1
    // in the segment's time.
    struct Segment {
        bool linear = true;
        double step = 1.0;         // linear: range per sample
        double coefficient = 0.0;  // curved
        double overshoot = 1.0;
    };
    static Segment segment(double ms, double sampleRate, double curve) {
        const double samples = std::max(1.0, ms * 0.001 * sampleRate);
        Segment s;
        if (curve <= 0.0) {
            s.step = 1.0 / samples;
            return s;
        }
        s.linear = false;
        s.overshoot = std::pow(10.0, 1.0 - 4.0 * curve);  // 10 .. 0.001
        s.coefficient = std::exp(-std::log((1.0 + s.overshoot) / s.overshoot) / samples);
        return s;
    }
    // One sample toward `to`, along a segment that runs the whole range.
    double step(const Segment& s, double to, bool rising) {
        if (s.linear) {
            level_ += rising ? s.step : -s.step;
            return level_;
        }
        // Aimed past the end by the overshoot (of the whole range): above 1
        // for the attack, below sustain for the decay, below 0 for the
        // release.
        const double aim = rising ? to + s.overshoot : to - s.overshoot;
        level_ = aim + (level_ - aim) * s.coefficient;
        return level_;
    }

    Segment attack_, decay_, release_;
    Stage stage_ = Stage::Idle;
    double level_ = 0.0, sustain_ = 1.0;
    double attackCurve_ = 0.0, curve_ = 0.0;
    double attackMs_ = 1.0, decayMs_ = 1.0, releaseMs_ = 1.0, sampleRate_ = 48000.0;
};

}  // namespace supple::dsp
