// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// SDK-free eight-voice test instrument. All processing state is fixed-size.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "supple/base/rng.h"

#if __has_cpp_attribute(clang::nonblocking)
#define PROBESYNTH_RT [[clang::nonblocking]]
#else
#define PROBESYNTH_RT
#endif

namespace probesynth {
inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g) { return 20.0 * std::log10(std::max(g, 1.0e-12)); }
class Processor {
  public:
    struct Params {
        int waveform = 0;  // saw, square, sine, noise
        double cutoff = 4000.0, resonance = 0.2, filterAmount = 2.0;
        int filterSlope = 0;  // 0: 12 dB, 1: 24 dB
        double ampAttack = 5, ampDecay = 100, ampSustain = 0.7, ampRelease = 200;
        double filterAttack = 5, filterDecay = 150, filterSustain = 0.2, filterRelease = 250;
        double outputGainDb = -12;
        uint32_t seed = 1;
    };
    struct Telemetry {
        double inputPeakDb = -120, outputPeakDb = -120;
    };
    void prepare(double sr, int channels) {
        sampleRate_ = std::max(sr, 1.0);
        channels_ = std::clamp(channels, 1, 2);
        reset();
    }
    void setParams(const Params& p) {
        if (p.seed != params_.seed) noise_.reseed(p.seed);
        params_ = p;
    }
    void reset() {
        voices_ = {};
        age_ = 0;
        peak_ = 0;
        noise_.reseed(params_.seed);
    }
    int latencySamples() const { return 0; }
    void noteOn(int noteId, int channel, int key, double velocity) PROBESYNTH_RT {
        Voice* v = nullptr;
        for (auto& voice : voices_)
            if (!voice.active) {
                v = &voice;
                break;
            }
        if (!v)
            v = &*std::min_element(voices_.begin(), voices_.end(),
                                   [](const Voice& a, const Voice& b) { return a.age < b.age; });
        *v = {};
        v->active = true;
        v->id = noteId;
        v->channel = channel;
        v->key = key;
        v->velocity = std::clamp(velocity, 0.0, 1.0);
        v->age = ++age_;
    }
    void noteOff(int noteId, int channel, int key) PROBESYNTH_RT {
        for (auto& v : voices_)
            if (matches(v, noteId, channel, key)) v.gate = false;
    }
    void noteChoke(int noteId, int channel, int key) PROBESYNTH_RT {
        for (auto& v : voices_)
            if (matches(v, noteId, channel, key)) v.active = false;
    }
    template <typename T>
    void process(T* const* output, int frames, bool captureMeters = false) PROBESYNTH_RT {
        if (!output || frames <= 0) return;
        const double gain = dbToGain(params_.outputGainDb);
        for (int i = 0; i < frames; ++i) {
            double sum = 0;
            for (auto& v : voices_) {
                if (!v.active) continue;
                const double a = envelope(v.amp, v.gate, params_.ampAttack, params_.ampDecay,
                                          params_.ampSustain, params_.ampRelease);
                const double f =
                    envelope(v.filter, v.gate, params_.filterAttack, params_.filterDecay,
                             params_.filterSustain, params_.filterRelease);
                if (v.amp.stage == Env::Off) {
                    v.active = false;
                    continue;
                }
                const double hz = 440.0 * std::exp2((v.key - 69.0) / 12.0);
                const double dt = std::min(hz / sampleRate_, 0.49);
                double osc = 0;
                switch (params_.waveform) {
                    case 1:
                        osc = (v.phase < 0.5 ? 1.0 : -1.0) + polyBlep(v.phase, dt) -
                              polyBlep(std::fmod(v.phase + 0.5, 1.0), dt);
                        break;
                    case 2:
                        osc = std::sin(6.283185307179586 * v.phase);
                        break;
                    case 3:
                        osc = noise_.bipolar();
                        break;
                    default:
                        osc = 2.0 * v.phase - 1.0 - polyBlep(v.phase, dt);
                        break;
                }
                v.phase += dt;
                if (v.phase >= 1.0) v.phase -= 1.0;
                const double cutoff = std::clamp(
                    params_.cutoff * std::exp2(params_.filterAmount * f), 20.0, sampleRate_ * 0.45);
                // TPT state-variable lowpass. A second identical section gives 24 dB.
                const double g = std::tan(3.141592653589793 * cutoff / sampleRate_);
                const double k = 2.0 - 1.9 * std::clamp(params_.resonance, 0.0, 1.0);
                double y = svf(osc, v.ic1, v.ic2, g, k);
                if (params_.filterSlope) y = svf(y, v.ic3, v.ic4, g, k);
                sum += y * a * v.velocity;
            }
            const double y = sum * gain * 0.2;
            for (int c = 0; c < channels_; ++c)
                if (output[c]) output[c][i] = static_cast<T>(y);
            if (captureMeters) peak_ = std::max(peak_, std::abs(y));
        }
    }
    Telemetry takeTelemetry() {
        Telemetry t{-120, gainToDb(peak_)};
        peak_ = 0;
        return t;
    }

  private:
    struct Env {
        enum Stage { Off, Attack, Decay, Sustain, Release } stage = Attack;
        double level = 0;
    };
    struct Voice {
        bool active = false, gate = true;
        int id = -1, channel = 0, key = 60;
        double velocity = 0, phase = 0, ic1 = 0, ic2 = 0, ic3 = 0, ic4 = 0;
        uint64_t age = 0;
        Env amp{}, filter{};
    };
    static bool matches(const Voice& v, int id, int channel, int key) {
        return v.active && (id < 0 || v.id == id) && (channel < 0 || v.channel == channel) &&
               (key < 0 || v.key == key);
    }
    double envelope(Env& e, bool gate, double attack, double decay, double sustain,
                    double release) {
        if (!gate && e.stage != Env::Off && e.stage != Env::Release) e.stage = Env::Release;
        const auto step = [&](double ms) {
            return 1.0 / (sampleRate_ * std::max(ms, 0.01) * .001);
        };
        switch (e.stage) {
            case Env::Attack:
                e.level += step(attack);
                if (e.level >= 1) {
                    e.level = 1;
                    e.stage = Env::Decay;
                }
                break;
            case Env::Decay:
                e.level -= step(decay) * (1 - sustain);
                if (e.level <= sustain) {
                    e.level = sustain;
                    e.stage = Env::Sustain;
                }
                break;
            case Env::Sustain:
                e.level = sustain;
                break;
            case Env::Release:
                e.level -= step(release);
                if (e.level <= 0) {
                    e.level = 0;
                    e.stage = Env::Off;
                }
                break;
            case Env::Off:
                break;
        }
        return e.level;
    }
    static double polyBlep(double phase, double dt) {
        if (phase < dt) {
            const double t = phase / dt;
            return t + t - t * t - 1;
        }
        if (phase > 1 - dt) {
            const double t = (phase - 1) / dt;
            return t * t + t + t + 1;
        }
        return 0;
    }
    static double svf(double x, double& ic1, double& ic2, double g, double k) {
        const double a = 1.0 / (1.0 + g * (g + k));
        const double v1 = a * (ic1 + g * (x - ic2));
        const double v2 = ic2 + g * v1;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return v2;
    }
    Params params_{};
    std::array<Voice, 8> voices_{};
    double sampleRate_ = 48000, peak_ = 0;
    int channels_ = 2;
    uint64_t age_ = 0;
    supple::Rng noise_{1};
};
}  // namespace probesynth
