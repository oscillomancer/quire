// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "stretch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "signalsmith-stretch/signalsmith-stretch.h"

namespace supple::engine {

namespace {
// The stretch randomizes the phase of new content once the ratio passes 2x,
// so the cache would not be reproducible with the library's default
// std::random_device seed.
constexpr long kStretchSeed = 20260928;
}  // namespace

Result<audio::AudioData> stretchAudio(const audio::AudioData& in, double playrate,
                                      double pitchSemitones) {
    if (in.channelCount() == 0 || in.frames() == 0)
        return Error("engine.stretch", "there is no audio to stretch", {},
                     "the source file has no frames");
    if (!(playrate > 0.0) || !std::isfinite(playrate))
        return Error("engine.stretch", "the playrate is not positive", {},
                     "playrate is 0.01 .. 100");
    if (in.frames() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Error("engine.stretch", "the source is too long to stretch in one pass", {},
                     "files up to ~12 hours at 48 kHz");
    const auto inputFrames = static_cast<int>(in.frames());
    const auto outputFrames = static_cast<int>(std::llround(inputFrames / playrate));
    if (outputFrames < 1)
        return Error("engine.stretch", "the stretched source is empty", {},
                     "a lower playrate would give it at least one frame");

    signalsmith::stretch::SignalsmithStretch<float> stretch(kStretchSeed);
    stretch.presetDefault(static_cast<int>(in.channelCount()), static_cast<float>(in.sampleRate));
    stretch.setTransposeSemitones(static_cast<float>(pitchSemitones));

    audio::AudioData out;
    out.sampleRate = in.sampleRate;
    out.truncated = in.truncated;
    out.channels.assign(in.channelCount(),
                        std::vector<float>(static_cast<std::size_t>(outputFrames)));
    std::vector<const float*> inputs;
    std::vector<float*> outputs;
    inputs.reserve(in.channelCount());
    outputs.reserve(in.channelCount());
    for (std::size_t c = 0; c < in.channelCount(); ++c) {
        inputs.push_back(in.channels[c].data());
        outputs.push_back(out.channels[c].data());
    }
    // Sources shorter than the stretcher's own latency cannot be primed.
    if (!stretch.exact(inputs, inputFrames, outputs, outputFrames))
        return Error("engine.stretch", "the source is shorter than the stretcher's window", {},
                     "a source of at least ~150 ms");
    return out;
}

}  // namespace supple::engine
