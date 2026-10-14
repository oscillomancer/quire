#include <supple/script.h>

// Three effects in the frequency domain: a gate that keeps only the parts of
// the spectrum above a threshold (a crude noise reduction), a robot voice
// (every phase zeroed: each frame repeats as a buzz at the hop rate) and a
// whisper (every phase random: pitch dissolves into breath).
enum class Mode { Gate, Robot, Whisper };

struct Spectral : Effect {
    static constexpr Info info = {
        .name = "Spectral", .id = "com.example.spectral", .vendor = "Example"};

    Param mode = choice("Mode", {"Gate", "Robot", "Whisper"}, 0);
    Param threshold =
        decibels("Threshold", -100, 0, -60).help("Gate: frequencies quieter than this go.");
    Param mix = percent("Mix", 100).smoothed(20);

    Array<Stft, 2> stft;
    Array<DelayLine, 2> dry;  // the input, delayed as much as the transform delays
    Random random;
    Mode effect = Mode::Gate;

    void prepare() {
        for (int c = 0; c < 2; ++c) {
            stft[c].prepare(1024, 4);
            dry[c].prepare(1024);
        }
        setLatency(stft[0].latency());
        random.seed(7);
    }

    void update() { effect = static_cast<Mode>(mode.index()); }

    // Changes one frame's spectrum.
    void change(Stft& s) {
        for (int k = 0; k < s.bins(); ++k) {
            switch (effect) {
                case Mode::Gate:
                    if (gainToDb(s.magnitude(k)) < threshold) s.scale(k, 0.0);
                    break;
                case Mode::Robot:
                    s.setPolar(k, s.magnitude(k), 0.0);
                    break;
                case Mode::Whisper:
                    s.setPolar(k, s.magnitude(k), random.next() * twoPi);
                    break;
            }
        }
    }

    void process(Block& io) {
        // Consume the shared random stream in sample/channel order, so
        // whispering is the same at every host block size.
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < io.channels && c < 2; ++c) {
                Stft& s = stft[c];
                DelayLine& d = dry[c];
                const double x = io[c][i];
                if (s.push(x)) change(s);
                const double wet = s.pull();
                d.push(x);
                io[c][i] = lerp(d.read(s.latency()), wet, mix.at(i));
            }
        }
    }
};
