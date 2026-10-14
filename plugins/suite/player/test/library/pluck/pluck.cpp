// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A plucked sine, for the player's tests: an instrument with the
// one-argument startVoice and no stopVoice.
struct Pluck : Instrument {
    static constexpr Info info = {.name = "Pluck", .id = "com.example.pluck", .voices = 4};

    Param decay = milliseconds("Decay", 10, 2000, 200).help("How long a note rings.");
    Param level = decibels("Level", -48, 0, -6).help("The output level.");

    struct Voice {
        Oscillator osc;
        Adsr env;
    };
    Array<Voice, 4> voice;

    void update() {
        for (Voice& v : voice) v.env.set(1, decay, 0, decay);
    }

    void startVoice(int v) { voice[v].env.gate(true); }
    void releaseVoice(int v) { voice[v].env.gate(false); }

    void process(Block& io) {
        const double gain = dbToGain(level);
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            for (int i = 0; i < io.frames; ++i) {
                voice[v].osc.setFrequency(voiceHz(v, i));
                const double tone = voice[v].osc.next();
                const double x = tone * voice[v].env.next() * voiceVelocity(v) * gain;
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!voice[v].env.active()) voiceFinished(v);
        }
    }
};
