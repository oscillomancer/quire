// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <array>
#include "supple/base/spsc.h"
#include "supple/engine/realtime.h"
namespace supple::engine::detail {
inline constexpr std::size_t kMidiInputPorts = 16;
inline constexpr std::size_t kMidiQueueSize = 2048;
struct TimedMidi {
    double seconds = 0;  // source monotonic clock
    std::uint8_t bytes[3]{};
    std::uint8_t size = 0;
};
// Fixed storage, allocated before any device opens. One OS producer per port;
// one audio consumer. Names are finalized before audio starts; OS producers
// use only their assigned queue index, so partial opens can compact the list.
class MidiInputBuffer {
  public:
    std::vector<std::string> names;
    std::vector<MidiInputWarning> warnings;
    // Startup only, after closing a failed source and before reusing its index.
    void discard(std::size_t port);
    void push(std::size_t port, double seconds, std::uint32_t message) SUPPLE_RT;
    void drop(std::uint64_t count = 1) SUPPLE_RT {
        dropped_.fetch_add(count, std::memory_order_relaxed);
    }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // Capture the preceding device period [end - frames/rate, end]. Late and
    // future timestamps clamp to its endpoints. Snapshot each ring once so a
    // busy producer cannot extend the callback's work indefinitely.
    void collect(double end, double rate, std::uint32_t frames) SUPPLE_RT;
    std::span<const MidiInputEvent> chunk(std::uint32_t offset, std::uint32_t frames) SUPPLE_RT;

  private:
    std::array<SpscRing<TimedMidi, kMidiQueueSize>, kMidiInputPorts> queues_;
    std::atomic<std::uint64_t> dropped_{0};
    std::array<MidiInputEvent, kMaxMidiInputEvents> period_{}, block_{};
    std::size_t count_ = 0, next_ = 0;
};
class MidiSource {
  public:
    virtual ~MidiSource() = default;
    // Best-effort per input; skipped devices go in buffer.warnings. Any source-
    // wide error is nonfatal to audio. stop joins callbacks before buffer destruction.
    virtual Status start(MidiInputBuffer&) = 0;
    virtual void stop() = 0;
    virtual double now() const SUPPLE_RT = 0;
    // Monitor only. Topology changes are advisory; keep servicing opened inputs.
    virtual bool changed() = 0;
};
std::unique_ptr<MidiSource> nativeMidiSource();
// WinMM's 32-bit milliseconds since start wrap after 49.7 days. QPC elapsed
// selects the nearest wrap epoch, without mutable state in the MIDI callback.
double winMidiSeconds(std::uint32_t milliseconds, double anchor, double now) SUPPLE_RT;
}  // namespace supple::engine::detail
