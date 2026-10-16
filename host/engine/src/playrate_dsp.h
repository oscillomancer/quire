// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "supple/base/rt.h"

namespace supple::engine::detail {

inline constexpr int kRateQuantum = 64;

// Fractional timeline samples, distinct from device/stream frames. Advance
// once per device sample, regardless of how callers partition their buffers.
// An authored envelope replaces the scalar (and never acquires its ramp).
class PlayrateClock {
  public:
    struct State {
        double timeline = 0, scalar = 1, from = 1, target = 1;
        std::uint32_t rampFrames = 0, rampAt = 0, waitFrames = 0, reserved = 0;
    };
    State state() const SUPPLE_RT {
        return {timeline_, scalar_, from_, target_, rampFrames_, rampAt_, waitFrames_, 0};
    }
    void restore(State state) SUPPLE_RT {
        timeline_ = state.timeline;
        scalar_ = state.scalar;
        from_ = state.from;
        target_ = state.target;
        rampFrames_ = state.rampFrames;
        rampAt_ = state.rampAt;
        waitFrames_ = state.waitFrames;
    }
    using Envelope = double (*)(const void*, double) noexcept;
    void reset(double timeline, double rate) SUPPLE_RT;
    void setRate(double rate, std::uint32_t rampFrames) SUPPLE_RT;
    void transition(double rate, double from, std::uint32_t rampFrames,
                    std::uint32_t waitFrames) SUPPLE_RT;
    void setEnvelope(Envelope envelope, const void* context) SUPPLE_RT;
    double timeline() const SUPPLE_RT { return timeline_; }
    double rate() const SUPPLE_RT;
    // Writes the boundary BEFORE each frame, then the final boundary.
    void advance(std::span<double> boundaries) SUPPLE_RT;
    // First device boundary reaching a timeline point; size() means beyond it.
    static std::size_t crossing(std::span<const double> boundaries, double timeline) SUPPLE_RT;

  private:
    double timeline_ = 0, scalar_ = 1, from_ = 1, target_ = 1;
    std::uint32_t rampFrames_ = 0, rampAt_ = 0, waitFrames_ = 0;
    Envelope envelope_ = nullptr;
    const void* context_ = nullptr;
};

// A reader supplies decoded source frames (zero outside the file, wrapping
// inside the reader when the take loops). Neither reader nor converter owns
// files or touches the media cache. Stereo phase/consumption is always linked.
struct RateSource {
    // Return a stable contiguous span when available, otherwise fill scratch
    // (padding/wrapping at file boundaries). One indirect call per channel
    // per quantum, never per tap or source frame.
    using Fetch = const float* (*)(const void*, std::size_t channel, std::int64_t first,
                                   std::span<float> scratch) noexcept;
    const void* context = nullptr;
    Fetch fetch = nullptr;
    std::size_t channels = 0;
};

class TapeReader;
// Original polyphase sinc table, shared by every source in a plan.
class TapeTable {
  public:
    TapeTable();

  private:
    friend class TapeReader;
    static constexpr int kTaps = 256, kPhases = 1024, kBanks = 13;
    std::vector<float> coefficients_;
};

// Any channel count; one shared phase and cutoff trajectory. Scratch is sized
// before publication. positions contains 65 monotonically increasing source
// boundaries, each increment in [.25,4]. Output is 64 frames per channel.
class TapeReader {
  public:
    TapeReader(std::shared_ptr<const TapeTable> table, std::size_t channels);
    bool process(RateSource source, std::span<const double, kRateQuantum + 1> positions,
                 std::span<float* const> output) SUPPLE_RT;

  private:
    std::shared_ptr<const TapeTable> table_;
    std::size_t channels_;
    std::vector<float> scratch_;
};

// Signalsmith wrapper with fixed 64-output-frame calls and explicit reseeding.
// Construction/configuration is control-thread only. Seek/priming/processing
// reuse all storage. Priming is cooperative: each prime() discards at most
// `quanta` canonical blocks, allowing a callback-owned source to spread work.
// The owner chooses the transition boundary AFTER ready(), and crossfades
// there. It must not publish incomplete output as stretched audio.
class StretchReader {
  public:
    explicit StretchReader(double sampleRate, std::size_t channels = 2);
    ~StretchReader();
    StretchReader(StretchReader&&) noexcept;
    StretchReader& operator=(StretchReader&&) noexcept;
    StretchReader(const StretchReader&) = delete;
    StretchReader& operator=(const StretchReader&) = delete;

    void seek(RateSource source, double position, double rate) SUPPLE_RT;
    void prime(RateSource source, std::uint32_t quanta) SUPPLE_RT;
    bool primeTo(RateSource source, double end) SUPPLE_RT;
    bool ready() const SUPPLE_RT;
    int analysisLookahead() const SUPPLE_RT;  // source frames
    int lookahead() const SUPPLE_RT;          // device frames
    double inputPosition() const SUPPLE_RT;
    // end is timeline(quantum end + lookahead()) + analysisLookahead(), in
    // source frames. It must advance by 16..256 source frames.
    // Returns false (without advancing state) if the trajectory violates that
    // bound, e.g. a loop discontinuity requiring seek(). Output is always 64.
    bool process(RateSource source, double end, std::span<float* const> output) SUPPLE_RT;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace supple::engine::detail
