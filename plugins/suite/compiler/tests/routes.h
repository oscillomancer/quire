// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Modulation: sources the script sets, parameters routes move (one past its
// own range), and a factory patch's routes, one through an instrument's own
// source; a file that starts with a factory file, and a patch that picks
// another.
struct Routes : Instrument {
    static constexpr Info info = {.name = "Routes", .id = "com.example.routes", .voices = 4};

    Param cutoff = frequency("Cutoff", 20, 20000, 1000).polyModulated();
    Param fine = cents("Fine", -100, 100, 0).polyModulated().routeRange(-1200, 1200);
    Param mix = percent("Mix", 100).modulated();
    Source lfo = voiceSource("LFO").help("A wobble.");
    Source env = voiceSource("Env").unipolar().withKey("env2");
    Source macro = source("Macro").unipolar();
    AudioFile sample = audioFile("Sample").withDefault("factory:pad.flac");

    void factoryPatches(Patches& p) {
        p.add("Wobble")
            .route("lfo>cutoff", 1.5)
            .route("env2>fine", 700, "modwheel")
            .file(sample, "factory:keys.flac");
    }
    void startVoice(int v) { lfo.set(v, 0.5); }
    void releaseVoice(int v) {}
    void process(Block& io) { macro.set(mix.routed(0)); }
};
