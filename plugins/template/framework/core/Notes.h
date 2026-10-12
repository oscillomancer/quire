// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Notes and MIDI as the processor hears them: what note(), midi() and
// noteExpression() carry (core/ProcessorBase.h). Independent of the product,
// so the DSP library's voice manager (dsp/Voices.h) can use them too.
#pragma once

#include <cmath>
#include <cstdint>

namespace supple {

struct NoteEvent {
    enum class Type : uint8_t { On, Off, Choke };
    Type type = Type::On;
    int16_t key = 60;      // 0..127
    int16_t channel = 0;   // 0..15
    int16_t port = 0;
    int32_t noteId = -1;   // CLAP note id, -1 when unknown (MIDI)
    double velocity = 1.0; // 0..1
};

// Which notes an event names, as CLAP addresses them: every field it gives
// must match, and -1 gives none (matches anything). A release from MIDI has
// no note ID, so it matches by key and channel.
struct NoteAddress {
    int32_t noteId = -1;
    int16_t port = -1, channel = -1, key = -1;

    static NoteAddress of(const NoteEvent& e) { return {e.noteId, e.port, e.channel, e.key}; }
    bool matches(const NoteEvent& voice) const {
        return (noteId < 0 || noteId == voice.noteId) && (key < 0 || key == voice.key) &&
               (channel < 0 || channel == voice.channel) && (port < 0 || port == voice.port);
    }
};

// A MIDI 1.0 channel message other than a note: controllers, pitch bend,
// pressure, program changes. Notes from MIDI arrive as NoteEvents instead.
struct MidiMessage {
    uint8_t data[3] = {0, 0, 0};
    int16_t port = 0;

    int status() const { return data[0] & 0xF0; }
    int channel() const { return data[0] & 0x0F; }
    bool isController() const { return status() == 0xB0; }
    int controller() const { return data[1]; }  // the CC number
    int value() const { return data[2]; }       // the CC value, 0..127
    bool isPitchBend() const { return status() == 0xE0; }
    double pitchBend() const { return ((data[2] << 7 | data[1]) - 8192) / 8192.0; }  // -1 .. +1, 0 centred
    bool isChannelPressure() const { return status() == 0xD0; }
    bool isPolyPressure() const { return status() == 0xA0; }  // key data[1], pressure data[2]
    double pressure() const { return (isPolyPressure() ? data[2] : data[1]) / 127.0; }
    bool isProgramChange() const { return status() == 0xC0; }

    static MidiMessage make(int status, int channel, int a, int b = 0) {
        MidiMessage m;
        m.data[0] = static_cast<uint8_t>((status & 0xF0) | (channel & 0x0F));
        m.data[1] = static_cast<uint8_t>(a & 0x7F);
        m.data[2] = static_cast<uint8_t>(b & 0x7F);
        return m;
    }
    static MidiMessage cc(int channel, int number, int value) { return make(0xB0, channel, number, value); }
    // `bend` -1..+1.
    static MidiMessage bend(int channel, double bend) {
        const double clamped = bend < -1.0 ? -1.0 : bend > 1.0 ? 1.0 : bend;
        int v = static_cast<int>(std::lround(8192.0 + clamped * 8192.0));
        v = v < 0 ? 0 : v > 16383 ? 16383 : v;
        return make(0xE0, channel, v & 0x7F, v >> 7);
    }
};

// A per-note expression (CLAP note expressions; MPE and VST3 note
// expressions arrive as these). Values: Volume a linear gain 0..4, Pan 0..1
// (0.5 centre), Tuning in semitones, the rest 0..1. A voice starts at
// neutral (gain 1, centre, 0) and keeps each expression until it changes.
struct NoteExpression {
    enum class Type : uint8_t { Volume, Pan, Tuning, Vibrato, Expression, Brightness, Pressure };
    Type type = Type::Volume;
    NoteAddress address;
    double value = 0.0;
};

}  // namespace supple
