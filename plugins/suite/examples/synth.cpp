#include <supple/script.h>

// A polyphonic synth: a saw per voice through a low-pass filter and an
// envelope. With Glide above 0 it plays one note at a time, sliding.
struct Synth : Instrument {
    static constexpr Info info = {.name = "Synth", .id = "com.example.synth", .voices = 8};

    Param attack = milliseconds("Attack", 1, 2000, 5).help("How fast a note rises.");
    Param release =
        milliseconds("Release", 5, 5000, 300).help("How long a note fades once its key is up.");
    Param cutoff = frequency("Cutoff", 200, 16000, 3000).help("The low-pass filter's corner.");
    Param glide = milliseconds("Glide", 0, 1000, 0)
                      .help("The slide from note to note; above 0, one note at a time.");
    Param level = decibels("Level", -48, 0, -12).smoothed(20).help("The output level.");

    struct Voice {
        Oscillator osc;
        Adsr env;
        Biquad filter;
    };
    Array<Voice, 8> voice;

    void prepare() {
        for (Voice& v : voice) v.osc.setShape(Oscillator::Shape::Saw);
    }

    void update() {
        for (Voice& v : voice) {
            v.env.set(attack, 200, 0.7, release);
            v.filter.lowpass(cutoff, 0.707);
        }
        setVoiceMode(glide > 0 ? VoiceMode::Legato : VoiceMode::Poly);
        setGlide(glide);
    }

    // Voice v starts a note. retrigger is false when it glides on from the
    // note before (Legato): its envelope keeps going.
    void startVoice(int v, bool retrigger) {
        if (retrigger) voice[v].env.gate(true);
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }

    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            Voice& s = voice[v];
            const double velocity = voiceVelocity(v);
            for (int i = 0; i < io.frames; ++i) {
                s.osc.setFrequency(voiceHz(v, i));
                const double tone = s.filter.process(s.osc.next());
                const double x = tone * s.env.next() * velocity * dbToGain(level.at(i));
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!s.env.active()) voiceFinished(v);
        }
    }
};
