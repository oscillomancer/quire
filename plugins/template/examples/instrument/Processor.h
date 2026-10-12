// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: an eight-voice instrument, kept small so the shape of an
// instrument is easy to see. Replace it with the real thing; keep the shape.
//
//   - notes arrive through note(), at their exact sample, between blocks,
//     and MIDI through midi(); a voice manager (dsp/Voices.h) decides which
//     voice plays each: allocation and stealing, the sustain pedal, Mono
//     and Legato with Glide, and each channel's pitch bend (Bend Range);
//   - it keeps CLAP's books: every note handed in is reported over, once,
//     with the address it started with, when its voice dies away, is stolen
//     or choked, or reset() ends it -- voiceEnded() below;
//   - the sound stays here, in an array beside the manager's, one entry per
//     voice: an envelope (dsp/Envelope.h), an oscillator, the per-note
//     expressions;
//   - the mod wheel (CC 1) adds vibrato from an LFO (dsp/Oscillators.h);
//   - note expressions (product.cmake: tuning, volume) move one note's pitch
//     and level, and a host's per-note modulation of Level (polyModulatable)
//     one voice's level, each kept from its event until the voice ends;
//   - kVoices and kOverlappingNotes tell the host about the voices, and
//     voiceCount() how many the mode can use;
//   - microtuning (PRODUCT_TUNING): each voice asks retuning() at every block,
//     so a tuning that moves while a note is held moves the note;
//   - waveforms: a sine; a triangle, naive live, which aliases at high
//     notes, and offline (kOfflineMode) summed from its harmonics below
//     Nyquist, band-limited, which the tests measure; and a saw, made
//     band-limited live with polyBLEP;
//   - everything is fixed storage sized in the class: nothing allocates.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/Dsp.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "dsp/Envelope.h"
#include "dsp/Oscillators.h"
#include "dsp/Voices.h"
#include "Params.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    static constexpr int kVoices = 8;
    // A second note on a held key gets a voice of its own, and a release by
    // note ID ends only its own.
    static constexpr bool kOverlappingNotes = true;
    // Bounces (CLAP render, VST3 offline processing) get the band-limited
    // triangle: block.offline.
    static constexpr bool kOfflineMode = true;
    static constexpr double kVibratoHz = 5.5;
    static constexpr double kVibratoSemitones = 0.5;  // at full mod wheel

    struct Telemetry {
        int16_t keys[kVoices];  // -1 for a silent voice
    };
    static constexpr double kTelemetryHz = 60.0;

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        level_.setTime(20.0, sampleRate_);
        levelStep_ = 1.0 - std::exp(-1.0 / (0.02 * sampleRate_));
        voices_.prepare(sampleRate_);
        vibrato_.setRate(kVibratoHz, sampleRate_);
        reset();
    }

    void reset() {
        frame_ = 0;
        voices_.reset(*this);
        for (Voice& v : sound_) v = Voice{};
        wheel_ = 0.0;
        vibrato_.reset();
    }

    void update(const supple::ParamValues& v) {
        frame_ = 0;  // mode changes retire stacked notes at this update
        level_.set(supple::dsp::dbToGain(v[kLevel]), v.initial());
        levelDb_ = v[kLevel];
        waveform_ = v.choice(kWaveform);
        for (Voice& voice : sound_) voice.envelope.setTimes(v[kAttack], v[kDecay], v[kSustain], v[kRelease], sampleRate_);
        releaseSeconds_ = v[kRelease] * 0.001;
        voices_.setBendRange(v[kBendRange]);
        voices_.setMode(static_cast<Voices::Mode>(v.choice(kMode)), *this);
        voices_.setGlide(v[kGlide]);
        // A voice's modulated level follows the parameter under it.
        for (Voice& voice : sound_) voice.levelTarget = levelOffset(voice.levelMod);
    }
    int voiceCount() const { return voices_.voiceCount(); }

    void note(const supple::NoteEvent& e) {
        frame_ = 0;
        voices_.note(e, *this);
    }

    void midi(const supple::MidiMessage& m) {
        frame_ = 0;
        voices_.midi(m, *this);  // bend, the sustain pedal
        if (m.isController() && m.controller() == 1) wheel_ = m.value() / 127.0;
    }

    void noteExpression(const supple::NoteExpression& x) {
        using Type = supple::NoteExpression::Type;
        for (int k = 0; k < kVoices; ++k) {
            if (!voices_[k].active || !x.address.matches(voices_[k].note)) continue;
            if (x.type == Type::Tuning) sound_[k].tuning = x.value;
            if (x.type == Type::Volume) sound_[k].volume = std::clamp(x.value, 0.0, 4.0);
        }
    }

    void noteModulation(const supple::NoteModulation& m) {
        if (m.id != kLevel) return;
        for (int k = 0; k < kVoices; ++k) {
            if (!voices_[k].active || !m.address.matches(voices_[k].note)) continue;
            sound_[k].levelMod = m.amount;
            sound_[k].levelTarget = levelOffset(m.amount);
        }
    }

    void process(supple::Block& b) {
        // The tuning, once a block: often enough to follow one that moves.
        for (int k = 0; k < kVoices; ++k)
            if (voices_[k].active) sound_[k].retune = retuning(voices_[k].note.key, voices_[k].note.channel);
        const bool bandLimited = b.offline && waveform_ == 1;
        for (int i = 0; i < b.frames; ++i) {
            frame_ = i;
            const double vibrato = wheel_ * kVibratoSemitones * vibrato_.tick();
            voices_.tick();  // glide
            double sum = 0.0;
            for (int k = 0; k < kVoices; ++k) {
                if (!voices_[k].active) continue;
                Voice& v = sound_[k];
                const double envelope = v.envelope.tick();
                if (!v.envelope.active()) {
                    voices_.finished(k, *this);  // died away: the note is over
                    continue;
                }
                const double pitch = voices_.pitch(k) - 69 + v.retune + v.tuning + vibrato;
                const double hz = 440.0 * std::exp2(pitch / 12.0);
                // The oscillator is a polyBLEP saw; the other shapes read its phase.
                v.osc.setFrequency(hz, sampleRate_);
                const double phase = v.osc.phase(), saw = v.osc.tick();
                // The per-voice level glides like the global one (about 20 ms).
                v.levelGain += (v.levelTarget - v.levelGain) * levelStep_;
                const double osc = waveform_ == 2 ? saw
                                   : waveform_ == 1 ? (bandLimited ? bandLimitedTriangle(phase, hz) : 1.0 - 4.0 * std::fabs(phase - 0.5))
                                                    : std::sin(2.0 * supple::dsp::kPi * phase);
                sum += osc * envelope * voices_[k].velocity * v.volume * v.levelGain;
            }
            const double y = sum * level_.next() * 0.5;
            for (int c = 0; c < b.channels; ++c) b.channel(c)[i] = y;
        }
    }

    uint32_t tailSamples() const { return static_cast<uint32_t>(std::ceil(releaseSeconds_ * sampleRate_)); }

    void captureTelemetry(Telemetry& t) const {
        for (int k = 0; k < kVoices; ++k) t.keys[k] = voices_[k].active ? voices_[k].note.key : int16_t{-1};
    }

    // --- what the voice manager asks of the sound (dsp/Voices.h) -------------------

    void voiceStart(int k, bool retrigger) {
        Voice& v = sound_[k];
        // A new note starts neutral; a legato one keeps its envelope and phase.
        v.tuning = 0.0;
        v.volume = 1.0;
        v.levelMod = 0.0;
        v.levelTarget = levelOffset(0.0);
        if (retrigger) v.envelope.gate(true);
        if (!v.started) {
            v.osc.setPhase(0.0);
            v.levelGain = v.levelTarget;
            v.started = true;
        }
    }
    void voiceRelease(int k) { sound_[k].envelope.gate(false); }
    void voiceStop(int k) {
        sound_[k].envelope.kill();
        sound_[k].started = false;
    }
    void voiceEnded(const supple::NoteEvent& note) {
        noteEnded(note, frame_);
        for (int k = 0; k < kVoices; ++k)
            if (!voices_[k].active) sound_[k].started = false;
    }

  private:
    using Voices = supple::dsp::Voices<kVoices>;

    // The triangle above as its Fourier series, -8/pi^2 * sum cos(2 pi k phase)
    // / k^2 over odd k, stopping below Nyquist: nothing to alias. The cosines
    // come from a recurrence, one multiply-add per harmonic, but a low note
    // still has hundreds of them. Below 20 Hz use the naive shape: this caps
    // work at the harmonic count needed by 20 Hz, even if tuning underflows
    // the frequency to zero.
    double bandLimitedTriangle(double phase, double hz) const {
        if (hz < 20.0) return 1.0 - 4.0 * std::fabs(phase - 0.5);
        const double theta = 2.0 * supple::dsp::kPi * phase;
        const double c2 = 2.0 * std::cos(2.0 * theta);
        double previous = std::cos(theta), current = previous;  // cos(-theta), cos(theta)
        double sum = 0.0;
        for (int k = 1; k * hz < 0.5 * sampleRate_; k += 2) {
            sum += current / (static_cast<double>(k) * k);
            const double next = c2 * current - previous;
            previous = current;
            current = next;
        }
        return -8.0 / (supple::dsp::kPi * supple::dsp::kPi) * sum;
    }

    // The sound of one voice; which note it plays is the manager's.
    struct Voice {
        supple::dsp::Adsr envelope;
        supple::dsp::Oscillator osc;  // a saw: its phase serves every waveform
        bool started = false;         // sounding since its last start from silence
        double tuning = 0.0, volume = 1.0;  // note expressions
        double retune = 0.0;                // microtuning, semitones
        double levelMod = 0.0;              // per-note modulation of Level, host units
        double levelTarget = 1.0, levelGain = 1.0;  // its gain relative to Level
    };

    // The gain a per-note modulation of Level adds to the level everyone has.
    double levelOffset(double amount) const {
        if (amount == 0.0) return 1.0;
        const double db = supple::modulatedPlain(*supple::params::find(kLevel), levelDb_, amount);
        return supple::dsp::dbToGain(db - levelDb_);
    }

    double sampleRate_ = 48000.0;
    supple::dsp::Smoother level_;
    double levelDb_ = -6.0;
    Voices voices_;
    Voice sound_[kVoices];
    int waveform_ = 0;
    double releaseSeconds_ = 0.25;
    double wheel_ = 0.0;
    supple::dsp::Lfo vibrato_;
    double levelStep_ = 0.001;
    int frame_ = 0;  // the sample being rendered, for note ends
};

}  // namespace product
