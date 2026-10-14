// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A compressor that can listen to another track. With a signal on its
// sidechain input (a kick drum, a voice), that signal decides how far the
// main one is turned down: ducking. With none, it listens to its own input.
struct Compressor : Effect {
    static constexpr Info info = {.name = "Compressor",
                                  .id = "com.example.compressor",
                                  .vendor = "Example",
                                  .sidechain = true};

    Param threshold = decibels("Threshold", -60, 0, -24).help("Where turning down begins.");
    // A member may not share a factory's name, so the key is given.
    Param slope = ratio("Ratio", 1, 20, 4).withKey("ratio").help("dB in for each dB out above it.");
    Param attack = milliseconds("Attack", 0.1, 100, 10).help("How fast it turns down.");
    Param release = milliseconds("Release", 10, 2000, 150).help("How fast it lets go.");
    Param makeup = decibels("Makeup", 0, 24, 0).smoothed(20).help("Gain after compressing.");
    Param reduction = decibels("Reduction", -40, 0, 0).meter().help("How far it turns down now.");

    EnvelopeFollower follower;

    void update() { follower.set(attack, release); }

    void process(Block& io) {
        double most = 0.0;
        for (int i = 0; i < io.frames; ++i) {
            // The louder channel sets the level, so the stereo image holds still.
            double peak = 0.0;
            for (int c = 0; c < io.channels; ++c)
                peak = max(peak, abs(io.sidechainConnected ? io.sidechain(c)[i] : io[c][i]));
            const double level = follower.process(peak);
            const double changeDb = compressorGainDb(gainToDb(level), threshold, slope, 6.0);
            const double gain = dbToGain(changeDb + makeup.at(i));
            for (int c = 0; c < io.channels; ++c) io[c][i] *= gain;
            most = min(most, changeDb);
        }
        reduction.set(max(most, -40.0));
    }
};
