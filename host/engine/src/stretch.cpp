// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "stretch.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include "signalsmith-stretch/signalsmith-stretch.h"
#include "supple/dsp/dsp.h"

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

Result<audio::AudioData> warpAudio(const audio::AudioData& in, const model::Take& mapping,
                                   std::size_t frames, const std::function<bool()>& cancelled) {
    dsp::DenormalGuard denormals;
    SUPPLE_TRY(model::validateWarpMap(mapping));
    const auto channels = in.channelCount();
    if (channels == 0 || channels > 128 || frames > (std::size_t{1} << 28) / channels)
        return Error("warp.budget", "warped audio exceeds the channel-frame budget", {},
                     "use a shorter item or fewer linked channels");
    audio::AudioData out;
    out.sampleRate = in.sampleRate;
    out.channels.assign(channels, std::vector<float>(frames));
    auto sample = [&](std::size_t c, std::int64_t n) {
        return n >= 0 && static_cast<std::size_t>(n) < in.frames()
                   ? in.channels[c][static_cast<std::size_t>(n)]
                   : 0.f;
    };
    bool identity = mapping.pitch == 0 && mapping.playrate == 1;
    for (const auto& m : mapping.stretchMarkers)
        identity = identity && std::abs(m.sourcePos - mapping.startOffset - m.itemPos) < 1e-12;
    if (identity) {
        const auto offset =
            static_cast<std::int64_t>(std::llround(mapping.startOffset * in.sampleRate));
        for (std::size_t c = 0; c < channels; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                out.channels[c][i] = sample(c, offset + static_cast<std::int64_t>(i));
        return out;
    }
    // Signalsmith phase-locks channels spectrally, but its low-energy fallback
    // can decorrelate exactly duplicated/polarity-reversed channels. Retain
    // those exact relationships; hashes only shortlist, samples prove equality.
    std::vector<std::uint64_t> hashes(channels);
    std::vector<float> signs(channels, 1);
    std::vector<std::size_t> representatives(channels);
    for (std::size_t c = 0; c < channels; ++c) {
        const auto nonzero = std::find_if(in.channels[c].begin(), in.channels[c].end(),
                                          [](float value) { return value != 0; });
        if (nonzero != in.channels[c].end() && *nonzero < 0) signs[c] = -1;
        auto hash = std::uint64_t{14695981039346656037ULL};
        for (float value : in.channels[c]) {
            const float normalized = value == 0 ? 0.f : value * signs[c];
            hash = (hash ^ std::bit_cast<std::uint32_t>(normalized)) * 1099511628211ULL;
        }
        hashes[c] = hash;
        representatives[c] = c;
        for (std::size_t other = 0; other < c; ++other) {
            const float sign = signs[c] * signs[other];
            if (hash == hashes[other] &&
                std::equal(in.channels[c].begin(), in.channels[c].end(), in.channels[other].begin(),
                           [sign](float a, float b) { return a == sign * b; })) {
                representatives[c] = representatives[other];
                break;
            }
        }
    }
    signalsmith::stretch::SignalsmithStretch<float> stretch(kStretchSeed);
    stretch.presetDefault(static_cast<int>(channels), static_cast<float>(in.sampleRate));
    stretch.setTransposeSemitones(static_cast<float>(mapping.pitch));
    constexpr int quantum = 64;
    const int priming = ((stretch.outputLatency() + quantum - 1) / quantum) * quantum;
    const model::AudioWarpMap timeMap(mapping);
    auto inputAt = [&](std::int64_t output) {
        return static_cast<std::int64_t>(
            std::floor(timeMap.sourceAt(static_cast<double>(output + stretch.outputLatency()) /
                                        in.sampleRate) *
                           in.sampleRate +
                       stretch.inputLatency()));
    };
    auto cursor = inputAt(-priming);
    std::vector<std::vector<float>> scratch(channels), block(channels, std::vector<float>(quantum));
    std::vector<const float*> inputs(channels);
    std::vector<float*> outputs(channels);
    auto fetch = [&](std::int64_t first, int count) {
        for (std::size_t c = 0; c < channels; ++c) {
            scratch[c].resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
                scratch[c][static_cast<std::size_t>(i)] = sample(c, first + i);
            inputs[c] = scratch[c].data();
        }
    };
    fetch(cursor - stretch.inputLatency(), stretch.inputLatency());
    const double initialRate = static_cast<double>(inputAt(-priming + quantum) - cursor) / quantum;
    stretch.seek(inputs, stretch.inputLatency(), initialRate);
    for (std::size_t c = 0; c < channels; ++c) outputs[c] = block[c].data();
    for (std::int64_t pos = -priming; pos < static_cast<std::int64_t>(frames); pos += quantum) {
        if (cancelled && cancelled())
            return Error("job.cancelled", "audio preparation cancelled", {},
                         "retry for the current version");
        const auto end = inputAt(pos + quantum);
        if (end < cursor || end - cursor > quantum * 100 + 1)
            return Error("warp.rate", "warp consumption exceeds the supported rate", {},
                         "use segment rates 0.01..100");
        fetch(cursor, static_cast<int>(end - cursor));
        stretch.process(inputs, static_cast<int>(end - cursor), outputs, quantum);
        cursor = end;
        if (pos >= 0)
            for (std::size_t c = 0; c < channels; ++c)
                std::copy_n(block[c].begin(),
                            std::min<std::size_t>(quantum, frames - static_cast<std::size_t>(pos)),
                            out.channels[c].begin() + pos);
    }
    for (std::size_t c = 0; c < channels; ++c)
        if (const auto reference = representatives[c]; reference != c) {
            const float sign = signs[c] * signs[reference];
            for (std::size_t n = 0; n < frames; ++n)
                out.channels[c][n] = sign * out.channels[reference][n];
        }
    return out;
}

}  // namespace supple::engine
