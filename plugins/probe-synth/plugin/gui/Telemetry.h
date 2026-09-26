// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace probesynth::gui {

struct Frame {
    float inputPeakDb = -120.0f;
    float outputPeakDb = -120.0f;
};

template <typename T, int CapacityPow2>
class Ring {
  public:
    static constexpr size_t kCapacity = size_t{1} << CapacityPow2;
    static constexpr size_t kMask = kCapacity - 1;

    bool push(const T& value) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= kCapacity) return false;  // never overwrite storage being copied
        data_[w & kMask] = value;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool peek(T& value) const {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        value = data_[r & kMask];
        return true;
    }
    void consume() {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        if (r != w) read_.store(r + 1, std::memory_order_release);
    }
    bool pop(T& value) {
        if (!peek(value)) return false;
        consume();
        return true;
    }
    size_t free() const {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        return kCapacity - (w - r);
    }
    bool pending() const {
        return read_.load(std::memory_order_relaxed) !=
               write_.load(std::memory_order_acquire);
    }

  private:
    T data_[kCapacity]{};
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
};

// 250 Hz gives responsive meters without fragmenting the audio callback.
using TelemetryRing = Ring<Frame, 8>;

struct ParamEdit {
    uint32_t id = 0;
    double value = 0.0;
    enum Kind : uint8_t { kBegin, kValue, kEnd } kind = kValue;
};
// A long drag may sit undrained while a host is stopped. Values are coalesced
// by the producer once only two slots remain; those slots are reserved for the
// final exact value and gesture end.
using ParamRing = Ring<ParamEdit, 12>;

// When a stopped/unresponsive host leaves fewer than three ordinary queue
// slots, a whole gesture is deferred as one item rather than publishing a
// BEGIN that cannot be followed by its exact final value and END.
struct ParamGesture {
    uint32_t id = 0;
    double value = 0.0;
};
using GestureRing = Ring<ParamGesture, 12>;

class EditorHost {
  public:
    virtual ~EditorHost() = default;
    virtual double paramValue(uint32_t id) const = 0;
    virtual void beginEdit(uint32_t id) = 0;
    virtual void setParam(uint32_t id, double value) = 0;
    virtual void endEdit(uint32_t id) = 0;
    virtual TelemetryRing& telemetry() = 0;
    virtual int latencySamples() const = 0;
    virtual double sampleRate() const = 0;
};

}  // namespace probesynth::gui
