// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: a convolution reverb. What it shows is a file, from the
// property to the audio thread:
//
//   - Properties.h declares the impulse response as an audioFile() property:
//     the framework relocates a moved file when a session loads, reports a
//     missing one, and loads it on its own thread;
//   - loadFile() runs there, never on the audio thread: it decodes the
//     file's first kMaxSeconds (the framework's default, resampled to the
//     session's rate), scales it, and builds the whole convolution,
//     partitions and FFTs, so the audio thread only swaps it in;
//   - file() swaps it in, at the start of a block; null (no file, a missing
//     one, one that failed) means the built-in room, made in prepare();
//   - the old room is cut off when the new one arrives, its last sample
//     faded out over a few milliseconds and the new one faded in, so the
//     change does not click;
//   - pre-delay through a fractional delay line (Hermite), so sweeping it
//     glides rather than steps; a low-pass on the room alone;
//   - no latency: the convolution's first partition is computed directly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "dsp/AudioFile.h"
#include "dsp/Convolution.h"
#include "dsp/Filters.h"
#include "dsp/Interpolation.h"
#include "dsp/Oscillators.h"
#include "Params.h"
#include "Properties.h"

namespace product {

// An impulse response ready to play.
struct Impulse {
    supple::dsp::Convolution convolution;
    double seconds = 0.0;
    int channels = 0;  // the file's
};

class Processor : public supple::ProcessorBase<Processor> {
  public:
    static constexpr double kMaxSeconds = 10.0;   // longer responses are cut short
    static constexpr double kRoomSeconds = 1.6;   // the built-in room
    static constexpr double kMaxPredelayMs = 200.0;

    // --- the loading thread ------------------------------------------------------

    using FileContents = Impulse;
    static std::unique_ptr<Impulse> loadFile(const supple::FileRequest& r, std::string& error) {
        auto audio = ProcessorBase::loadFile(r, error, kMaxSeconds);  // its start, at the session's rate
        if (!audio) return nullptr;
        return build(*audio, r.channels, error);
    }
    static std::string describeFile(const Impulse& i) {
        char line[64];
        std::snprintf(line, sizeof(line), "%s, %.2f s", i.channels == 1 ? "mono" : "stereo", i.seconds);
        return line;
    }

    // Cut to kMaxSeconds and scaled to unit energy (its loudest channel's),
    // so one response is about as loud as another.
    static std::unique_ptr<Impulse> build(supple::dsp::AudioData& audio, int channels, std::string& error) {
        const size_t frames = std::min(audio.frames(), static_cast<size_t>(kMaxSeconds * audio.sampleRate));
        double energy = 0.0;
        for (auto& c : audio.channel) {
            c.resize(frames);
            double e = 0.0;
            for (float v : c) e += static_cast<double>(v) * v;
            energy = std::max(energy, e);
        }
        if (energy <= 0.0) {
            error = "the response is silent";
            return nullptr;
        }
        const double scale = 1.0 / std::sqrt(energy);
        for (auto& c : audio.channel)
            for (float& v : c) v = static_cast<float>(v * scale);
        auto impulse = std::make_unique<Impulse>();
        impulse->convolution.prepare(audio.data().data(), audio.channels(), frames, channels);
        impulse->seconds = static_cast<double>(frames) / audio.sampleRate;
        impulse->channels = audio.channels();
        return impulse;
    }

    // The built-in room: noise in each channel (its own, so the room is
    // wide), falling 60 dB over kRoomSeconds and darkening as it goes.
    static supple::dsp::AudioData room(double sampleRate) {
        supple::dsp::AudioData a;
        a.sampleRate = sampleRate;
        const size_t frames = static_cast<size_t>(kRoomSeconds * sampleRate);
        const double fall = std::pow(0.001, 1.0 / static_cast<double>(frames));
        for (int c = 0; c < 2; ++c) {
            supple::dsp::Random random(static_cast<uint64_t>(c) + 1);
            supple::dsp::OnePole dark;
            supple::dsp::OnePole::State s;
            std::vector<float> h(frames);
            double level = 1.0;
            for (size_t n = 0; n < frames; ++n, level *= fall) {
                dark.setCutoff(12000.0 * std::pow(2000.0 / 12000.0, static_cast<double>(n) / static_cast<double>(frames)), sampleRate);
                h[n] = static_cast<float>(level * dark.lowpass(s, random.bipolar()));
            }
            a.channel.push_back(std::move(h));
        }
        return a;
    }

    // --- the main thread, stopped ------------------------------------------------

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        channels_ = setup.channels;
        supple::dsp::AudioData a = room(sampleRate_);
        std::string unused;
        room_ = build(a, channels_, unused);
        current_ = room_.get();
        predelay_.assign(static_cast<size_t>(channels_), {});
        for (auto& d : predelay_) d.prepare(static_cast<int>(kMaxPredelayMs * 0.001 * sampleRate_) + 4);
        tone_.assign(static_cast<size_t>(channels_), {});
        wet_.assign(static_cast<size_t>(setup.maxBlockSize), 0.0);
        ghost_.assign(static_cast<size_t>(channels_), 0.0);
        last_.assign(static_cast<size_t>(channels_), 0.0);
        ghostFall_ = std::exp(-1.0 / (0.003 * sampleRate_));
        fadeStep_ = 1.0 / (0.02 * sampleRate_);
        reset();
    }

    // --- the audio thread ----------------------------------------------------------

    void reset() {
        for (auto& d : predelay_) d.clear();
        for (auto& s : tone_) s = {};
        if (room_) room_->convolution.reset();
        if (current_) current_->convolution.reset();
        std::fill(ghost_.begin(), ghost_.end(), 0.0);
        std::fill(last_.begin(), last_.end(), 0.0);
        fade_ = 1.0;
        silent_ = true;
    }

    void update(const supple::ParamValues& v) {
        toneOff_ = v[kTone] >= supple::params::find(kTone)->max - 1e-6;  // at the top, no filter at all
        toneFilter_ = supple::dsp::Biquad::lowpass(v[kTone], 0.7071, sampleRate_);
    }

    void file(uint32_t id, Impulse* impulse) {
        if (id != kImpulse) return;
        Impulse* next = impulse ? impulse : room_.get();
        if (next == current_) return;
        current_ = next;
        current_->convolution.reset();
        if (silent_) return;  // nothing playing yet: nothing to fade
        // The old room stops here: its last sample fades out, the new fades in.
        for (size_t c = 0; c < ghost_.size(); ++c) ghost_[c] = last_[c];
        fade_ = 0.0;
    }

    void process(supple::Block& b) {
        const double* mix = b.smoothed(kMix);
        const double* predelay = b.smoothed(kPredelay);
        const double* output = b.smoothed(kOutput);
        const double samplesPerMs = 0.001 * sampleRate_;
        const double fadeAtStart = fade_;
        for (int c = 0; c < b.channels; ++c) {
            double* x = b.channel(c);
            double* wet = wet_.data();
            auto& delay = predelay_[static_cast<size_t>(c)];
            for (int i = 0; i < b.frames; ++i) wet[i] = delay.process(x[i], predelay[i] * samplesPerMs);
            current_->convolution.process(c, wet, b.frames);
            if (!toneOff_) toneFilter_.process(tone_[static_cast<size_t>(c)], wet, b.frames);
            double& ghost = ghost_[static_cast<size_t>(c)];
            double fade = fadeAtStart;
            for (int i = 0; i < b.frames; ++i) {
                if (fade < 1.0) fade = std::min(1.0, fade + fadeStep_);
                const double w = wet[i] * fade + ghost;
                ghost *= ghostFall_;
                x[i] = (x[i] + (w - x[i]) * mix[i]) * supple::dsp::dbToGain(output[i]);
                last_[static_cast<size_t>(c)] = w;
            }
            fade_ = fade;
        }
        silent_ = false;
    }

    uint32_t tailSamples() const {
        const double seconds = (current_ ? current_->seconds : 0.0) + kMaxPredelayMs * 0.001;
        return static_cast<uint32_t>(std::ceil(seconds * sampleRate_));
    }

  private:
    double sampleRate_ = 48000.0;
    int channels_ = 2;
    std::unique_ptr<Impulse> room_;
    Impulse* current_ = nullptr;  // room_, or the file's (the framework's to free)
    std::vector<supple::dsp::FractionalDelay> predelay_;
    supple::dsp::Biquad toneFilter_;
    std::vector<supple::dsp::Biquad::State> tone_;
    bool toneOff_ = true;
    std::vector<double> wet_;
    std::vector<double> ghost_, last_;
    double ghostFall_ = 0.99, fade_ = 1.0, fadeStep_ = 0.001;
    bool silent_ = true;  // nothing processed since prepare() or reset()
};

}  // namespace product
