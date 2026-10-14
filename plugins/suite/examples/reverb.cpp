// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A convolution reverb: the input played through a recording of a space's
// echo (an impulse response), after a pre-delay, with a tone control on the
// reverb and a dry/wet mix.
struct Reverb : Effect {
    // The tail: the 20 seconds of response a Convolver keeps, at up to 192 kHz.
    static constexpr Info info = {.name = "Convolution Reverb",
                                  .id = "com.example.convolution-reverb",
                                  .vendor = "Example",
                                  .tail = 3840000};

    Convolver room = impulseResponse("Room").help("A recording of a space's echo.");
    Param predelay = milliseconds("Pre-Delay", 0, 200, 10).help("Time before the reverb starts.");
    Param tone = frequency("Tone", 1000, 20000, 8000).help("A low-pass on the reverb.");
    Param mix = percent("Mix", 30).smoothed(20);

    Array<DelayLine, 2> pre;
    Array<OnePole, 2> damp;

    void prepare() {
        for (DelayLine& d : pre) d.prepare(toInt(sampleRate() * 0.2) + 4);
    }

    void update() {
        for (OnePole& f : damp) f.setCutoff(tone);
    }

    void process(Block& io) {
        const double delay = predelay * 0.001 * sampleRate();
        for (int c = 0; c < io.channels && c < 2; ++c)
            for (int i = 0; i < io.frames; ++i) {
                const double dry = io[c][i];
                pre[c].push(dry);
                const double wet = damp[c].lowpass(room.process(c, pre[c].read(delay)));
                io[c][i] = lerp(dry, wet, mix.at(i));
            }
    }
};
