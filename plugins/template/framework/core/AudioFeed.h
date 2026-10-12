// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The audio itself, for an editor's scope or spectrum analyser: a product
// declares `static constexpr bool kAudioFeed = true;` in its Processor, and
// while an editor is open the framework copies the main input (as the host
// gave it) and the main output (as the host receives it, after bypass), the
// first two channels of each, into this queue. The editor drains it into an
// AudioHistory (gui/AudioHistory.h) every tick.
//
// Lock-free, single producer (the audio thread), single consumer (the main
// thread). When the editor stalls and it fills, the newest audio is dropped.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>

namespace supple {

struct FeedFrame {
    float in[2];   // the main input; silence for an instrument
    float out[2];  // the main output; channel 1 repeats 0 for a mono bus
};

class AudioFeed {
  public:
    static constexpr size_t kCapacity = size_t{1} << 14;  // a third of a second at 48 kHz

    AudioFeed() : data_(new FeedFrame[kCapacity]) {}

    // Audio thread: up to n frames, fill(frame, k) writing each; returns how
    // many there was room for.
    template <class Fill>
    size_t produce(size_t n, Fill&& fill) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        n = std::min(n, kCapacity - (w - r));
        for (size_t k = 0; k < n; ++k) fill(data_[(w + k) & kMask], k);
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    // Main thread: up to max frames, oldest first.
    size_t read(FeedFrame* out, size_t max) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t n = std::min(max, w - r);
        for (size_t k = 0; k < n; ++k) out[k] = data_[(r + k) & kMask];
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    // Main thread, with the audio thread not producing (activation).
    void clear() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

  private:
    static constexpr size_t kMask = kCapacity - 1;
    std::unique_ptr<FeedFrame[]> data_;
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

}  // namespace supple
