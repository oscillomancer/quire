// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// What host-smoke plays: the player's own ID, a level and a meter.
struct Smoke : Effect {
    static constexpr Info info = {.name = "Smoke", .id = "com.suppleaudio.script-player"};

    Param gain = decibels("Gain", -24, 12, 0).smoothed(10).help("The level.");
    Param peak = decibels("Peak", -60, 6, -60).meter();

    void process(Block& io) {
        double loudest = 0.0;
        for (int c = 0; c < io.channels; ++c) {
            for (int i = 0; i < io.frames; ++i) {
                io[c][i] *= dbToGain(gain.at(i));
                loudest = max(loudest, abs(io[c][i]));
            }
        }
        peak.set(max(gainToDb(loudest), -60.0));
    }
};
