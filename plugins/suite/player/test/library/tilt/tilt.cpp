// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A tilt equaliser: one control raises the highs and lowers the lows around
// a pivot (or the reverse), with two shelves meeting at the pivot.
struct Tilt : Effect {
    static constexpr Info info = {.name = "Tilt", .id = "com.example.tilt", .vendor = "Example"};

    Param tilt = decibels("Tilt", -6, 6, 0).help("Positive brightens: highs up, lows down.");
    Param pivot = frequency("Pivot", 200, 5000, 1000).help("Where the two shelves meet.");
    Param output = decibels("Output", -12, 12, 0).smoothed(20);

    void factoryPatches(Patches& p) {
        p.add("Flat").set(tilt, 0).set(pivot, 1000).set(output, 0);
        p.add("Warm").set(tilt, -3).set(pivot, 800).set(output, 0);
        p.add("Air").set(tilt, 3).set(pivot, 2500).set(output, -1);
    }

    Array<Biquad, 2> low, high;

    void update() {
        for (int c = 0; c < 2; ++c) {
            low[c].lowShelf(pivot, 0.707, -tilt * 0.5);
            high[c].highShelf(pivot, 0.707, tilt * 0.5);
        }
    }

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c) {
            for (int i = 0; i < io.frames; ++i) {
                double x = high[c].process(low[c].process(io[c][i]));
                io[c][i] = x * dbToGain(output.at(i));
            }
        }
    }
};
