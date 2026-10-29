// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
//
// factory-sample-flac <in.wav> <out.flac>: the audio of a WAV as 16-bit
// FLAC, dithered with a fixed seed, for render.py's factory samples.
#include <cstdio>

#include "supple/audio/audio_file.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: factory-sample-flac <in.wav> <out.flac>\n");
        return 2;
    }
    auto audio = supple::audio::readAudio(argv[1]);
    if (!audio) {
        std::fprintf(stderr, "%s: %s\n", argv[1], audio.error().message.c_str());
        return 1;
    }
    supple::audio::FlacOptions options;
    options.bitsPerSample = 16;
    options.ditherSeed = 1;
    if (auto written = supple::audio::writeFlac(argv[2], *audio, options); !written) {
        std::fprintf(stderr, "%s: %s\n", argv[2], written.error().message.c_str());
        return 1;
    }
    return 0;
}
