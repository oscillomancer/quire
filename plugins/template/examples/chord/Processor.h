// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's "DSP": a note effect. Every note that arrives leaves as a
// chord built on it, the root at once and each note above it Strum later.
// No audio at all; this is the shape of a MIDI processor.
//
//   - notes arrive through note() at their exact sample and leave through
//     sendNote(), at a frame of the current Block when they are due later;
//   - a release ends exactly the notes its chord sent, and a strum still in
//     progress sends no more;
//   - every input note is retired with noteEnded() once released, so hosts
//     (the VST3 wrapper above all) can forget it;
//   - other MIDI (controllers, pitch bend) passes straight through midi();
//   - the Custom shape is text, a property, parsed on the audio thread in
//     property() without allocating;
//   - each key is named by the chord it plays (noteNames, CLAP note-name), so
//     a host's piano roll reads "C4 Major"; changing Shape renames them;
//   - everything is fixed storage, so nothing allocates.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "Params.h"
#include "Properties.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    static constexpr int kMaxNotes = 6;   // the largest chord
    static constexpr int kChords = 32;    // input notes held at once
    // Each shape's intervals above the root, in semitones.
    struct Shape {
        int count;
        int intervals[kMaxNotes];
    };
    static constexpr Shape kShapes[] = {
        {2, {0, 12}}, {2, {0, 7}}, {3, {0, 4, 7}}, {3, {0, 3, 7}}, {3, {0, 5, 7}}, {4, {0, 4, 7, 10}},
    };

    // The Custom shape from its text: whole numbers of semitones from -24 to
    // 48, separated by spaces or commas, the first kMaxNotes of them. Anything
    // else is skipped; nothing usable plays the note alone.
    static Shape parseIntervals(std::string_view text) {
        Shape s{0, {}};
        size_t i = 0;
        while (i < text.size() && s.count < kMaxNotes) {
            while (i < text.size() && (text[i] == ' ' || text[i] == ',')) ++i;
            const size_t start = i;
            while (i < text.size() && text[i] != ' ' && text[i] != ',') ++i;
            std::string_view token = text.substr(start, i - start);
            const bool negative = !token.empty() && token[0] == '-';
            if (negative) token.remove_prefix(1);
            if (token.empty() || token.size() > 2) continue;
            int value = 0;
            bool digits = true;
            for (char c : token) {
                digits = digits && c >= '0' && c <= '9';
                value = value * 10 + (c - '0');
            }
            if (negative) value = -value;
            if (digits && value >= -24 && value <= 48) s.intervals[s.count++] = value;
        }
        if (s.count == 0) s = {1, {0}};
        return s;
    }

    struct Telemetry {
        int16_t keys[kMaxNotes];  // the last chord played, -1 past its end
    };
    static constexpr double kTelemetryHz = 30.0;

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        sampleRate_ = setup.sampleRate;
        reset();
    }

    void reset() {
        for (Chord& c : chords_) c = Chord{};
        now_ = 0;
        for (auto& k : last_) k = -1;
    }

    // Every key, by the chord it plays: "C4 Major". Main thread.
    static void noteNames(const supple::ParamValues& v, supple::NoteNames& names) {
        const char* shape = kShapeLabels[v.choice(kShape)];
        char key[8], text[32];
        for (int k = 0; k < 128; ++k) {
            supple::keyName(k, key, sizeof(key));
            std::snprintf(text, sizeof(text), "%s %s", key, shape);
            names.add(k, text);
        }
    }

    void update(const supple::ParamValues& v) {
        if (v.changed(kShape)) noteNamesChanged();
        shape_ = v.choice(kShape);
        strum_ = static_cast<int64_t>(std::llround(v[kStrum] * 0.001 * sampleRate_));
    }

    void property(uint32_t id, std::string_view value) {
        if (id == kIntervals) custom_ = parseIntervals(value);
    }

    void note(const supple::NoteEvent& e) {
        using Type = supple::NoteEvent::Type;
        if (e.type == Type::On) {
            Chord* chord = free();
            if (!chord) {
                noteEnded(e);  // a rejected note still belongs to the host
                return;
            }
            *chord = Chord{};
            chord->held = true;
            chord->input = e;
            const Shape& s = shape_ == kCustomShape ? custom_ : kShapes[shape_];
            chord->count = 0;
            for (int k = 0; k < s.count; ++k) {
                const int key = e.key + s.intervals[k];
                if (key < 0 || key > 127) continue;
                supple::NoteEvent n = e;
                n.key = static_cast<int16_t>(key);
                n.noteId = chord->count == 0 ? e.noteId : -1;
                chord->notes[chord->count] = n;
                chord->due[chord->count] = now_ + chord->count * strum_;
                ++chord->count;
            }
            for (int k = 0; k < kMaxNotes; ++k) last_[k] = k < chord->count ? chord->notes[k].key : int16_t{-1};
            send(*chord, 0);  // the root, and the rest when there is no strum
            return;
        }
        const supple::NoteAddress address = supple::NoteAddress::of(e);
        for (Chord& c : chords_) {
            if (!c.held || !address.matches(c.input)) continue;
            // What was sent is ended; what was still to be strummed never starts.
            for (int k = 0; k < c.sent; ++k) {
                supple::NoteEvent n = c.notes[k];
                n.type = e.type;
                n.velocity = e.velocity;
                sendNote(n);
            }
            noteEnded(c.input);
            c = Chord{};
        }
    }

    void midi(const supple::MidiMessage& m) { sendMidi(m); }

    void process(supple::Block& b) {
        for (Chord& c : chords_)
            if (c.held) send(c, b.frames);
        now_ += b.frames;
    }

    void captureTelemetry(Telemetry& t) const {
        for (int k = 0; k < kMaxNotes; ++k) t.keys[k] = last_[k];
    }

  private:
    struct Chord {
        bool held = false;
        supple::NoteEvent input{};
        supple::NoteEvent notes[kMaxNotes];
        int64_t due[kMaxNotes]{};  // in samples since prepare or reset
        int count = 0, sent = 0;
    };

    // Sends every note of `c` due within the next `frames` (at once for 0).
    void send(Chord& c, int frames) {
        while (c.sent < c.count && c.due[c.sent] <= now_ + (frames > 0 ? frames - 1 : 0)) {
            sendNote(c.notes[c.sent], static_cast<int>(c.due[c.sent] - now_));
            ++c.sent;
        }
    }

    Chord* free() {
        for (Chord& c : chords_)
            if (!c.held) return &c;
        return nullptr;
    }

    double sampleRate_ = 48000.0;
    int shape_ = 2;
    int64_t strum_ = 0, now_ = 0;
    Shape custom_{1, {0}};
    Chord chords_[kChords];
    int16_t last_[kMaxNotes] = {-1, -1, -1, -1, -1, -1};
};

}  // namespace product
