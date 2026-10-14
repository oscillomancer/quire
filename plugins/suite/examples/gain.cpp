// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// The smallest useful effect: a level in dB, glided so that moving it does
// not click.
struct Gain : Effect {
    static constexpr Info info = {.name = "Gain", .id = "com.example.gain", .vendor = "Example"};

    Param gain = decibels("Gain", -60, 24, 0).smoothed(20).help("Level change, in dB.");

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));
    }
};
