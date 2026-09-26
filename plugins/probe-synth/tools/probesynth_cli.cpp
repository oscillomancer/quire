// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Offline note renderer for DSP inspection.
#include <probesynth/Processor.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "wav.h"
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: probesynth-cli <output.wav> [note] [seconds]\n");
        return 2;
    }
    const int note = argc > 2 ? std::atoi(argv[2]) : 60;
    const double seconds = argc > 3 ? std::atof(argv[3]) : 2.0;
    if (note < 0 || note > 127 || seconds <= 0 || seconds > 60) return 2;
    wavio::Audio audio;
    audio.channels = 2;
    audio.sampleRate = 48000;
    const size_t frames = static_cast<size_t>(seconds * 48000);
    audio.ch.resize(2);
    for (auto& ch : audio.ch) ch.resize(frames);
    probesynth::Processor p;
    p.prepare(48000, 2);
    p.setParams({});
    p.noteOn(1, 0, note, 0.8);
    for (size_t off = 0; off < frames; off += 256) {
        if (off >= 24000 && off < 24256) p.noteOff(1, 0, note);
        double* ptrs[] = {audio.ch[0].data() + off, audio.ch[1].data() + off};
        p.process(ptrs, static_cast<int>(std::min<size_t>(256, frames - off)));
    }
    std::string error;
    if (!wavio::write(argv[1], audio, 32, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s (%zu frames)\n", argv[1], frames);
    return 0;
}
