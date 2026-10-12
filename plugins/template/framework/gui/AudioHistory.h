// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The last stretch of the audio feed (core/AudioFeed.h), on the main thread:
// what a scope draws and a spectrum analyses. EditorBase keeps one for a
// processor that declared kAudioFeed and fills it every tick; products read
// it through EditorBase::audio().
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/AudioFeed.h"

namespace supple::gui {

// Which signal: the main input or output, a channel of it or both summed.
struct Signal {
    enum Bus : uint8_t { Input, Output };
    Bus bus = Output;
    int channel = -1;  // 0 or 1; -1 for the two averaged (mid)

    static constexpr Signal input(int channel = -1) { return {Input, channel}; }
    static constexpr Signal output(int channel = -1) { return {Output, channel}; }
};

class AudioHistory {
  public:
    static constexpr size_t kFrames = size_t{1} << 17;  // 2.7 seconds at 48 kHz

    AudioHistory() : frames_(kFrames, FeedFrame{}) {}

    void push(const FeedFrame* frames, size_t n) {
        for (size_t k = 0; k < n; ++k) frames_[(written_ + k) & kMask] = frames[k];
        written_ += n;
    }
    void clear() {
        std::fill(frames_.begin(), frames_.end(), FeedFrame{});
        written_ = 0;
    }

    // Frames pushed since the editor opened: grows by the sample rate each
    // second while the host plays.
    uint64_t written() const { return written_; }

    // A sample `age` frames before the newest (0 is the newest).
    float at(Signal s, size_t age) const {
        const FeedFrame& f = frames_[(written_ - 1 - std::min(age, kFrames - 1)) & kMask];
        const float* v = s.bus == Signal::Input ? f.in : f.out;
        return s.channel < 0 ? 0.5f * (v[0] + v[1]) : v[s.channel & 1];
    }
    // The newest n samples, oldest first.
    void latest(Signal s, float* out, size_t n) const {
        n = std::min(n, kFrames);
        for (size_t k = 0; k < n; ++k) out[k] = at(s, n - 1 - k);
    }

  private:
    static constexpr size_t kMask = kFrames - 1;
    std::vector<FeedFrame> frames_;
    uint64_t written_ = 0;
};

}  // namespace supple::gui
