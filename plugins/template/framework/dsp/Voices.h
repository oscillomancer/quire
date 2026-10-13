// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Which voice plays which note: allocation and stealing, the sustain pedal,
// mono and legato modes with portamento, pitch bend, and MPE. The sound of a
// voice stays the product's -- its oscillators and envelopes live in the
// product's own array, indexed the same way -- and this decides when each
// starts, releases and stops, and keeps CLAP's note-end bookkeeping exact:
// every note handed in is reported over exactly once.
//
// The processor passes a handler (usually itself) with four members:
//
//   void voiceStart(int v, bool retrigger);  // v plays voices[v].note now; retrigger false: a
//                                            // legato change of pitch, keep the envelope going
//   void voiceRelease(int v);                // the key is up: start the release
//   void voiceStop(int v);                   // silence at once (choke, steal)
//   void voiceEnded(const supple::NoteEvent& n);  // the note is over: noteEnded(n)
//
// and in process(), per sample:
//
//   voices_.tick();                                  // portamento
//   for each v with voices_[v].active:
//       hz = 440 * exp2((voices_.pitch(v) - 69) / 12);
//       ... render; once its envelope has died away:
//       voices_.finished(v, *this);                  // reports the note over
//
// A product that renders a block at a time reads pitchAt(v, i) for frame i
// and calls advance(frames) after the block instead of tick().
//
// Pitch is in semitones (MIDI key numbers, fractional): the key, glided,
// plus the bend for the voice's channel. N is the capacity; setCount()
// plays fewer, for a polyphony chosen at run time. Everything is fixed
// storage.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/Notes.h"

namespace supple::dsp {

template <int N>
class Voices {
  public:
    static constexpr int kCount = N;
    static constexpr int kHeldKeys = 16;  // mono's memory of keys held under the sounding one

    enum class Mode { Poly, Mono, Legato };
    // MPE (MIDI Polyphonic Expression): one zone, its master channel 1
    // (Lower) or 16 (Upper), every other channel a member carrying one
    // note's bend, pressure and timbre (CC 74).
    enum class Mpe { Off, Lower, Upper };

    struct Voice {
        bool active = false;  // sounding, from voiceStart until it ends
        bool held = false;    // its key is down, or the sustain pedal holds it
        bool pedal = false;   // its key is up and only the pedal holds it
        NoteEvent note{};     // the note it plays, as it arrived
        double velocity = 0.0;
        double key = 60.0;    // glided toward note.key
        double pressure = 0.0, timbre = 0.0;  // MPE, 0..1
        uint64_t age = 0;
        double glideStep = 0.0;
        int glideLeft = 0;
    };

    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        setGlide(glideMs_);
    }
    // A mode transition releases sounding keys and forgets the mono stack.
    // Their note ends still arrive through finished(), after the tails die.
    template <class H>
    void setMode(Mode m, H& h) {
        if (m == mode_) return;
        forgetHeld(h);
        for (int v = 0; v < N; ++v)
            if (voices_[v].active && voices_[v].held) release(v, h);
        lastKey_ = -1.0;
        mode_ = m;
    }
    Mode mode() const { return mode_; }
    // Portamento: each new note slides from the last one's pitch over this
    // long (0: none). In poly mode too.
    void setGlide(double ms) {
        glideMs_ = std::max(0.0, ms);
        glideSamples_ = static_cast<int>(std::lround(glideMs_ * 0.001 * sampleRate_));
    }
    // Semitones at full pitch bend: every channel, or MPE's master channel.
    void setBendRange(double semitones) { bendRange_ = semitones; }
    // MPE members' bend range is their own, 48 by the specification's default.
    void setMpe(Mpe zone, double memberBendRange = 48.0) {
        mpe_ = zone;
        memberRange_ = memberBendRange;
    }
    // Voices the current mode can use (CLAP voice-info).
    int voiceCount() const { return mode_ == Mode::Poly ? count_ : 1; }
    // Plays voices 0..n-1 only, n from 1 to N (N until this is called).
    // Voices beyond a smaller count are stopped.
    template <class H>
    void setCount(int n, H& h) {
        n = std::clamp(n, 1, N);
        for (int v = n; v < N; ++v)
            if (voices_[v].active) stop(v, h);
        count_ = n;
    }
    int count() const { return count_; }

    const Voice& operator[](int v) const { return voices_[v]; }
    // The voice's pitch now, in semitones: the key, glided, and bent.
    double pitch(int v) const { return voices_[v].key + bend(voices_[v].note.channel); }
    bool sustainPedal() const { return pedal_; }

    // Ends every note: stops each voice, then reports it over.
    template <class H>
    void reset(H& h) {
        forgetHeld(h);
        for (int v = 0; v < N; ++v)
            if (voices_[v].active) stop(v, h);
        for (double& b : bend_) b = 0.0;
        pedal_ = false;
        lastKey_ = -1.0;
    }

    template <class H>
    void note(const NoteEvent& e, H& h) {
        using Type = NoteEvent::Type;
        if (e.type == Type::On) {
            if (mode_ == Mode::Poly) polyOn(e, h);
            else monoOn(e, h);
        } else if (e.type == Type::Off) {
            if (mode_ == Mode::Poly) polyOff(e, h);
            else monoOff(e, h);
        } else {
            choke(e, h);
        }
    }

    // Sustain and CC120/123 are omni: they affect voices on all channels.
    // MPE accepts sustain from the master only, then applies it to the whole
    // zone. Outside MPE, sustain from any channel controls that same pedal.
    template <class H>
    void midi(const MidiMessage& m, H& h) {
        const int channel = m.channel();
        if (m.isPitchBend()) bend_[channel] = m.pitchBend();
        if (m.isController() && m.controller() == 64 && (mpe_ == Mpe::Off || channel == master())) {
            const bool down = m.value() >= 64;
            if (pedal_ && !down) {
                pedal_ = false;
                for (int v = 0; v < N; ++v)
                    if (voices_[v].active && voices_[v].pedal) release(v, h);
            }
            pedal_ = down;
        }
        // All notes off, all sound off.
        if (m.isController() && (m.controller() == 123 || m.controller() == 120)) {
            forgetHeld(h);
            for (int v = 0; v < N; ++v) {
                if (!voices_[v].active) continue;
                if (m.controller() == 120) stop(v, h);
                else if (voices_[v].held) release(v, h);
            }
        }
        if (mpe_ != Mpe::Off && channel != master()) {
            for (Voice& v : voices_) {
                if (!v.active || v.note.channel != channel) continue;
                if (m.isChannelPressure()) v.pressure = m.pressure();
                if (m.isController() && m.controller() == 74) v.timbre = m.value() / 127.0;
            }
        }
    }

    // Portamento, one sample on. Call once per sample while rendering.
    void tick() {
        for (Voice& v : voices_) {
            if (v.glideLeft <= 0) continue;
            v.key += v.glideStep;
            if (--v.glideLeft == 0) v.key = v.note.key;
        }
    }
    // For a block at a time: the voice's pitch `frame` samples on, and the
    // glide moved on by a block's `frames` once it is rendered. Within a
    // block the glide is a straight line, so pitchAt(v, frames) before
    // advance(frames) is pitch(v) after it.
    double pitchAt(int v, int frame) const {
        const Voice& x = voices_[v];
        const int steps = std::clamp(frame, 0, std::max(x.glideLeft, 0));
        const double key = x.glideLeft > 0 && steps == x.glideLeft ? x.note.key : x.key + x.glideStep * steps;
        return key + bend(x.note.channel);
    }
    void advance(int frames) {
        for (Voice& v : voices_) {
            if (v.glideLeft <= 0 || frames <= 0) continue;
            const int steps = std::min(frames, v.glideLeft);
            v.glideLeft -= steps;
            v.key = v.glideLeft == 0 ? v.note.key : v.key + v.glideStep * steps;
        }
    }

    // The voice's sound has died away (its release is over): the note is
    // reported over, at `frame` of the block when the handler passes it on.
    template <class H>
    void finished(int v, H& h) {
        if (!voices_[v].active) return;
        voices_[v].active = voices_[v].held = voices_[v].pedal = false;
        voices_[v].glideLeft = 0;
        h.voiceEnded(voices_[v].note);
    }

  private:
    int master() const { return mpe_ == Mpe::Upper ? 15 : 0; }
    double bend(int channel) const {
        channel &= 15;
        if (mpe_ == Mpe::Off) return bendRange_ * bend_[channel];
        if (channel == master()) return bendRange_ * bend_[channel];
        return memberRange_ * bend_[channel] + bendRange_ * bend_[master()];
    }
    // Mono's held keys, dropped; those not sounding are over now, and the
    // sounding one will be when its voice is.
    template <class H>
    void forgetHeld(H& h) {
        for (int k = 0; k < stack_; ++k)
            if (!sounding(held_[k])) h.voiceEnded(held_[k]);
        stack_ = 0;
    }
    bool sounding(const NoteEvent& n) const {
        for (const Voice& v : voices_)
            if (v.active && same(v.note, n)) return true;
        return false;
    }
    static bool same(const NoteEvent& a, const NoteEvent& b) {
        return a.key == b.key && a.channel == b.channel && a.port == b.port && a.noteId == b.noteId;
    }

    // A free voice, else the one released longest ago, else the oldest.
    int allocate() const {
        int best = 0;
        for (int v = 0; v < count_; ++v) {
            const Voice& c = voices_[v];
            if (!c.active) return v;
            const Voice& b = voices_[best];
            if ((!c.held && b.held) || (c.held == b.held && c.age < b.age)) best = v;
        }
        return best;
    }

    void begin(Voice& v, const NoteEvent& e, double from) {
        v.note = e;
        v.velocity = e.velocity;
        v.active = v.held = true;
        v.pedal = false;
        v.age = ++clock_;
        if (mpe_ != Mpe::Off) v.pressure = v.timbre = 0.0;
        glideTo(v, from);
        lastKey_ = e.key;
    }
    void glideTo(Voice& v, double from) {
        if (glideSamples_ > 0 && from >= 0.0 && from != v.note.key) {
            v.key = from;
            v.glideLeft = glideSamples_;
            v.glideStep = (v.note.key - from) / glideSamples_;
        } else {
            v.key = v.note.key;
            v.glideLeft = 0;
        }
    }

    template <class H>
    void stop(int v, H& h) {
        h.voiceStop(v);
        finished(v, h);
    }
    template <class H>
    void release(int v, H& h) {
        voices_[v].held = voices_[v].pedal = false;
        h.voiceRelease(v);
    }

    template <class H>
    void polyOn(const NoteEvent& e, H& h) {
        const int v = allocate();
        if (voices_[v].active) stop(v, h);  // stolen
        begin(voices_[v], e, lastKey_);
        h.voiceStart(v, true);
    }
    template <class H>
    void polyOff(const NoteEvent& e, H& h) {
        const NoteAddress address = NoteAddress::of(e);
        for (int v = 0; v < N; ++v) {
            Voice& voice = voices_[v];
            if (!voice.active || !voice.held || voice.pedal || !address.matches(voice.note)) continue;
            if (pedal_) voice.pedal = true;
            else release(v, h);
        }
    }

    // Mono and legato: voice 0 plays the newest held key; releasing it goes
    // back to the one under it.
    template <class H>
    void monoOn(const NoteEvent& e, H& h) {
        Voice& v = voices_[0];
        if (stack_ == kHeldKeys) {  // the oldest held key is forgotten
            if (!sounding(held_[0])) h.voiceEnded(held_[0]);
            std::copy(held_ + 1, held_ + stack_, held_);
            --stack_;
        }
        held_[stack_++] = e;
        const bool wasHeld = v.active && v.held;
        // The note playing until now is over unless its key is still down.
        if (v.active && !inStack(v.note)) h.voiceEnded(v.note);
        const double from = v.active ? v.key : lastKey_;
        begin(v, e, from);
        h.voiceStart(0, mode_ == Mode::Mono || !wasHeld);
    }
    template <class H>
    void monoOff(const NoteEvent& e, H& h) {
        const NoteAddress address = NoteAddress::of(e);
        Voice& v = voices_[0];
        bool soundingReleased = false;
        int kept = 0;
        for (int k = 0; k < stack_; ++k) {
            if (!address.matches(held_[k])) {
                held_[kept++] = held_[k];
                continue;
            }
            if (v.active && same(v.note, held_[k])) soundingReleased = true;
            else h.voiceEnded(held_[k]);
        }
        stack_ = kept;
        if (!soundingReleased || !v.held) return;
        if (stack_ > 0) {
            // Back to the newest key still down.
            h.voiceEnded(v.note);
            const double from = v.key;
            begin(v, held_[stack_ - 1], from);
            h.voiceStart(0, mode_ == Mode::Mono);
        } else if (pedal_) {
            v.pedal = true;
        } else {
            release(0, h);
        }
    }
    bool inStack(const NoteEvent& n) const {
        for (int k = 0; k < stack_; ++k)
            if (same(held_[k], n)) return true;
        return false;
    }

    template <class H>
    void choke(const NoteEvent& e, H& h) {
        const NoteAddress address = NoteAddress::of(e);
        // Held keys first: one that is sounding is reported by stop() below.
        int kept = 0;
        for (int k = 0; k < stack_; ++k) {
            if (!address.matches(held_[k])) held_[kept++] = held_[k];
            else if (!sounding(held_[k])) h.voiceEnded(held_[k]);
        }
        stack_ = kept;
        for (int v = 0; v < N; ++v)
            if (voices_[v].active && address.matches(voices_[v].note)) stop(v, h);
    }

    Voice voices_[N];
    NoteEvent held_[kHeldKeys];
    int stack_ = 0;
    int count_ = N;
    Mode mode_ = Mode::Poly;
    Mpe mpe_ = Mpe::Off;
    double sampleRate_ = 48000.0, glideMs_ = 0.0;
    int glideSamples_ = 0;
    double bendRange_ = 2.0, memberRange_ = 48.0;
    double bend_[16] = {};
    bool pedal_ = false;
    double lastKey_ = -1.0;
    uint64_t clock_ = 0;
};

}  // namespace supple::dsp
