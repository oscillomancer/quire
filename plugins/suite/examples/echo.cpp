#include <supple/script.h>

// A feedback delay: what comes back is fed in again, quieter each time.
struct Echo : Effect {
    static constexpr Info info = {
        .name = "Echo", .id = "com.example.echo", .vendor = "Example", .tail = -1};

    Param time = milliseconds("Time", 1, 2000, 350).help("Time between repeats.");
    Param feedback = percent("Feedback", 40, 0, 95).help("How much of each repeat returns.");
    Param mix = percent("Mix", 30).help("Dry to wet.");

    Array<DelayLine, 2> lines;

    void prepare() {
        // Two seconds at any sample rate, allocated here, never in process().
        for (auto& line : lines) line.prepare(toInt(sampleRate() * 2.0) + 4);
    }

    void process(Block& io) {
        const double delay = time * 0.001 * sampleRate();
        for (int c = 0; c < io.channels && c < 2; ++c) {
            DelayLine& line = lines[c];
            for (int i = 0; i < io.frames; ++i) {
                const double dry = io[c][i];
                const double wet = line.read(delay);
                line.push(dry + wet * feedback);
                io[c][i] = lerp(dry, wet, mix);
            }
        }
    }
};
