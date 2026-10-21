// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A polyphonic synth: a saw per voice through a low-pass filter and an
// envelope, and an LFO for the patch's routes to move the filter with. With
// Glide above 0 it plays one note at a time, sliding.
struct Synth : Instrument {
    static constexpr Info info = {.name = "Synth", .id = "com.example.synth", .voices = 8};

    Param attack = milliseconds("Attack", 1, 2000, 5).help("How fast a note rises.");
    Param release =
        milliseconds("Release", 5, 5000, 300).help("How long a note fades once its key is up.");
    Param cutoff = frequency("Cutoff", 200, 16000, 3000)
                       .polyModulated()
                       .help("The low-pass filter's corner, which routes move for each voice.");
    Param rate = frequency("LFO Rate", 0.1, 20, 4).help("How fast the LFO swings.");
    Param voices = integer("Voices", 1, 8, 8).help("How many notes sound at once.");
    Param glide = milliseconds("Glide", 0, 1000, 0)
                      .help("The slide from note to note; above 0, one note at a time.");
    Param level = decibels("Level", -48, 0, -12).smoothed(20).help("The output level.");

    // A sine for each note, from its start: "lfo>cutoff" in the patch's
    // routes makes a wah of it, "velocity>cutoff" opens harder notes.
    Source lfo = voiceSource("LFO").help("A sine for each note, from its start.");

    struct Voice {
        Oscillator osc;
        Adsr env;
        Biquad filter;
        double phase = 0.0;  // the LFO's
        int tick = 0;        // frames until the filter follows its routes again
    };
    Array<Voice, 8> voice;

    void factoryPatches(Patches& p) {
        p.add("Wah").set(cutoff, 900).route("lfo>cutoff", 1.5).route("velocity>cutoff", 1);
    }

    void prepare() {
        for (Voice& v : voice) v.osc.setShape(Oscillator::Shape::Saw);
    }

    void update() {
        for (Voice& v : voice) v.env.set(attack, 200, 0.7, release);
        setVoiceMode(glide > 0 ? VoiceMode::Legato : VoiceMode::Poly);
        setGlide(glide);
        setVoiceLimit(voices.index());
    }

    // Voice v starts a note. retrigger is false when it glides on from the
    // note before (Legato): its envelope and LFO keep going.
    void startVoice(int v, bool retrigger) {
        if (!retrigger) return;
        voice[v].env.gate(true);
        voice[v].phase = 0.0;
        voice[v].tick = 0;
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }

    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            Voice& s = voice[v];
            const double velocity = voiceVelocity(v);
            for (int i = 0; i < io.frames; ++i) {
                // Every 16 frames of the note: the LFO on, then the cutoff
                // with its routes from the sources as they are now.
                if (s.tick == 0) {
                    lfo.set(v, sin(twoPi * s.phase));
                    s.filter.lowpass(cutoff.forVoice(v), 0.707);
                    s.phase = fmod(s.phase + rate * 16 / sampleRate(), 1.0);
                }
                s.tick = (s.tick + 1) % 16;
                s.osc.setFrequency(voiceHz(v, i));
                const double tone = s.filter.process(s.osc.next());
                const double x = tone * s.env.next() * velocity * dbToGain(level.at(i));
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!s.env.active()) voiceFinished(v);
        }
    }
};
