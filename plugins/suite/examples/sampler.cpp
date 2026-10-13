#include <supple/script.h>

// A sampler: every key plays the sample, faster up the keyboard and slower
// down it; the root key plays it as recorded. Each voice reads the file at
// its own speed under an envelope, and ends with the sample.
struct Sampler : Instrument {
    static constexpr Info info = {
        .name = "Sampler", .id = "com.example.sampler", .vendor = "Example", .voices = 16};

    AudioFile sample = audioFile("Sample").help("The sound every key plays.");
    Param root = integer("Root Key", 0, 127, 60).help("The key that plays it as recorded.");
    Param attack = milliseconds("Attack", 0.5, 2000, 2).help("How fast a note rises.");
    Param release = milliseconds("Release", 5, 5000, 200).help("How fast it fades once let go.");
    Param level = decibels("Level", -48, 6, 0).smoothed(20).help("The output level.");

    struct Voice {
        double position = 0.0;  // in the file's frames
        Adsr env;
    };
    Array<Voice, 16> voice;

    void update() {
        for (Voice& v : voice) v.env.set(attack, 0, 1, release);
    }

    void startVoice(int v) {
        voice[v].position = 0.0;
        voice[v].env.gate(true);
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }

    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            Voice& s = voice[v];
            const double velocity = voiceVelocity(v);
            for (int i = 0; i < io.frames; ++i) {
                const double speed = exp2((voicePitch(v, i) - root) / 12.0);
                const double gain = s.env.next() * velocity * dbToGain(level.at(i));
                for (int c = 0; c < io.channels; ++c) io[c][i] += sample.read(c, s.position) * gain;
                s.position += speed;
            }
            if (!s.env.active() || s.position >= sample.frames()) voiceFinished(v);
        }
    }
};
