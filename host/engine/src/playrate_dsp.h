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
    using Envelope = double (*)(const void*, double) noexcept;
    void reset(double timeline, double rate) SUPPLE_RT;
    void setRate(double rate, std::uint32_t rampFrames) SUPPLE_RT;
    void setEnvelope(Envelope envelope, const void* context) SUPPLE_RT;
    double timeline() const SUPPLE_RT { return timeline_; }
    double rate() const SUPPLE_RT;
    // Writes the boundary BEFORE each frame, then the final boundary.
    void advance(std::span<double> boundaries) SUPPLE_RT;
    // First device boundary reaching a timeline point; size() means beyond it.
    static std::size_t crossing(std::span<const double> boundaries, double timeline) SUPPLE_RT;

  private:
    double timeline_ = 0, scalar_ = 1, from_ = 1, target_ = 1;
    std::uint32_t rampFrames_ = 0, rampAt_ = 0;
    Envelope envelope_ = nullptr;
    const void* context_ = nullptr;
};

// A reader supplies decoded source frames (zero outside the file, wrapping
// inside the reader when the take loops). Neither reader nor converter owns
// files or touches the media cache. Stereo phase/consumption is always linked.
struct RateSource {
    using Read = float (*)(const void*, int channel, std::int64_t frame) noexcept;
    const void* context = nullptr;
    Read read = nullptr;
    float at(int channel, std::int64_t frame) const SUPPLE_RT {
        return read(context, channel, frame);
    }
};

// Original, windowed-sinc polyphase tape reader. Construct/share on the
// control thread; all rate/phase coefficients are prepared before publication.
// Cutoff banks round UP in rate so rapid rate changes cannot alias.
class TapeTable {
  public:
    TapeTable();
    std::array<float, 2> read(RateSource source, double position, double rate) const SUPPLE_RT;

  private:
    static constexpr int kTaps = 256, kPhases = 1024, kBanks = 13;
    std::vector<float> coefficients_;
};

// Signalsmith wrapper with fixed 64-output-frame calls and explicit reseeding.
// Construction/configuration is control-thread only. Seek/priming/processing
// reuse all storage. Priming is cooperative: each prime() discards at most
// `quanta` canonical blocks, allowing a callback-owned source to spread work.
// The owner chooses the transition boundary AFTER ready(), and crossfades
// there. It must not publish incomplete output as stretched audio.
class StretchReader {
  public:
    explicit StretchReader(double sampleRate);
    ~StretchReader();
    StretchReader(StretchReader&&) noexcept;
    StretchReader& operator=(StretchReader&&) noexcept;
    StretchReader(const StretchReader&) = delete;
    StretchReader& operator=(const StretchReader&) = delete;

    void seek(RateSource source, double position, double rate) SUPPLE_RT;
    void prime(RateSource source, std::uint32_t quanta) SUPPLE_RT;
    bool ready() const SUPPLE_RT;
    int lookahead() const SUPPLE_RT;
    double inputPosition() const SUPPLE_RT;
    // end is the source position needed AFTER this quantum, including the
    // stretcher's source lookahead. It must advance by 16..256 source frames.
    // Returns false (without advancing state) if the trajectory violates that
    // bound, e.g. a loop discontinuity requiring seek(). Output is always 64.
    bool process(RateSource source, double end, std::span<float, kRateQuantum> left,
                 std::span<float, kRateQuantum> right) SUPPLE_RT;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace supple::engine::detail
