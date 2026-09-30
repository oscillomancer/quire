// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Offline WAV renderer for DSP development and independent measurements.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <delay/Processor.h>

#include "wav.h"

namespace {
struct Options {
    std::string input, output;
    delay::Processor::Params params;
    int block = 512;
    int bits = 32;
};

void usage() {
    std::printf(
        "delay-cli <input.wav> <output.wav> [options]\n\n"
        "  --input-gain <dB>   default 0\n"
        "  --output-gain <dB>  default 0\n"
        "  --mix <0..1>        default 1\n"
        "  --bypass\n"
        "  --block <frames>    default 512\n"
        "  --bits <24|32>      24-bit PCM or 32-bit float\n");
}

bool parse(int argc, char** argv, Options& options) {
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto number = [&](double& value) {
            if (++i >= argc) return false;
            value = std::atof(argv[i]);
            return true;
        };
        if (arg == "--help" || arg == "-h") { usage(); std::exit(0); }
        if (arg == "--input-gain") { if (!number(options.params.inputGainDb)) return false; }
        else if (arg == "--output-gain") { if (!number(options.params.outputGainDb)) return false; }
        else if (arg == "--mix") { if (!number(options.params.mix)) return false; }
        else if (arg == "--block") {
            double value = 0.0; if (!number(value)) return false; options.block = static_cast<int>(value);
        } else if (arg == "--bits") {
            double value = 0.0; if (!number(value)) return false; options.bits = static_cast<int>(value);
        } else if (arg == "--bypass") options.params.bypass = true;
        else if (!arg.empty() && arg[0] == '-') return false;
        else if (positional++ == 0) options.input = arg;
        else if (positional == 2) options.output = arg;
        else return false;
    }
    return positional == 2 && options.block > 0 &&
           (options.bits == 24 || options.bits == 32);
}

double samplePeak(const wavio::Audio& audio) {
    double peak = 0.0;
    for (const auto& channel : audio.ch)
        for (double sample : channel) peak = std::max(peak, std::abs(sample));
    return delay::gainToDb(peak);
}
}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) { usage(); return 2; }

    std::string error;
    wavio::Audio audio;
    if (!wavio::read(options.input, audio, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    if (audio.channels < 1 || audio.frames() == 0) return 1;
    const double inputPeak = samplePeak(audio);

    delay::Processor processor;
    processor.prepare(audio.sampleRate, audio.channels);
    processor.setParams(options.params);
    std::vector<double*> pointers(static_cast<size_t>(audio.channels));

    const auto start = std::chrono::steady_clock::now();
    for (size_t offset = 0; offset < audio.frames(); offset += options.block) {
        const int count = static_cast<int>(std::min(static_cast<size_t>(options.block),
                                                    audio.frames() - offset));
        for (int c = 0; c < audio.channels; ++c)
            pointers[static_cast<size_t>(c)] = audio.ch[static_cast<size_t>(c)].data() + offset;
        processor.process(pointers.data(), count);
    }
    const auto finish = std::chrono::steady_clock::now();

    if (!wavio::write(options.output, audio, options.bits, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const double seconds = std::chrono::duration<double>(finish - start).count();
    const double duration = static_cast<double>(audio.frames()) / audio.sampleRate;
    std::printf("%s -> %s\n", options.input.c_str(), options.output.c_str());
    std::printf("%d ch, %d Hz, %.2f s, block %d\n",
                audio.channels, audio.sampleRate, duration, options.block);
    std::printf("sample peak: %+.2f -> %+.2f dBFS\n", inputPeak, samplePeak(audio));
    std::printf("speed: %.0fx realtime\n", duration / std::max(seconds, 1.0e-9));
    return 0;
}
