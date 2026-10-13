// Offline rendering through a chain of CLAP instances (spec: docs/specs/
// sandbox.md §3). Deterministic: the same instances in the same state, the
// same request and the same settings produce bit-identical audio.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/result.h"
#include "supple/host/clap_host.h"

namespace supple::sandbox {

struct RenderSettings {
    double sampleRate = 48000.0;
    std::uint32_t blockSize = 256;
    double tempo = 120.0;  // BPM, constant (the sandbox has no tempo map)
    std::uint16_t numerator = 4, denominator = 4;
    double tailLimitSeconds = 5.0;    // hard cap on the tail after the input ends
    double silenceDb = -120.0;        // the tail ends after this much silence…
    double silenceHoldSeconds = 0.2;  // …held this long
    bool resetBeforeRender = true;    // clap reset() on every instance first
};

// An event at an absolute sample position, for one instance of the chain
// (index into the chain; notes normally target the first, an instrument).
struct TimedEvent {
    std::int64_t sample = 0;
    std::size_t target = 0;
    host::Event event;  // event.time is ignored
};

struct RenderRequest {
    std::vector<TimedEvent> events;  // any order; sorted internally (stable)
    const audio::AudioData* input =
        nullptr;  // for effect chains; resampled beforehand to the session rate
    // The musical length (the tail follows). 0 = the input length or the last
    // event + 1, whichever is later.
    std::int64_t lengthSamples = 0;
    // Optional key for the first stage, at the input's sample origin/rate.
    // Mono is duplicated; shorter keys are zero-padded, including the tail.
    const audio::AudioData* sidechain = nullptr;
};

struct RenderResult {
    audio::AudioData audio;        // stereo, latency-compensated
    std::uint32_t latency = 0;     // total chain latency that was compensated (samples)
    std::int64_t tailSamples = 0;  // samples rendered after lengthSamples
    bool tailCapped = false;       // the tail hit tailLimitSeconds
    double peakDbfs = -1000.0;
    std::vector<std::string> log;  // plugin log lines
};

// Activates inactive instances (at the settings' rate and block size),
// renders on a dedicated audio thread, and leaves the instances active.
// All instances must share one channel layout: stereo outputs; effects with
// stereo inputs.
Result<RenderResult> render(std::span<host::Instance* const> chain, const RenderSettings& settings,
                            const RenderRequest& request);

// Beat/bar transport for a block starting at `sample` (constant tempo/meter).
host::Transport transportAt(const RenderSettings& settings, std::int64_t sample);

}  // namespace supple::sandbox
