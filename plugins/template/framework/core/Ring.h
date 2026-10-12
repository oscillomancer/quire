// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Bounded single-producer, single-consumer queue.
//
// Each side owns one index; publication is release/acquire. A full queue
// refuses the new item rather than overwriting a slot the consumer may be
// copying. peek()/consume() let the consumer look at an item and only take it
// once it has been delivered onward (an output queue that refuses an event
// leaves it at the head, to be retried).
#pragma once

#include <atomic>
#include <cstddef>

namespace supple {

template <typename T, int CapacityPow2>
class Ring {
  public:
    static constexpr size_t kCapacity = size_t{1} << CapacityPow2;

    bool push(const T& value) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= kCapacity) return false;
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
        if (r != write_.load(std::memory_order_acquire)) read_.store(r + 1, std::memory_order_release);
    }

    bool pop(T& value) {
        if (!peek(value)) return false;
        consume();
        return true;
    }

    // Producer side: room for at least one more item. Only the producer
    // fills the queue, so the answer holds until it pushes.
    bool writable() const {
        return write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire) < kCapacity;
    }

    // Consumer side.
    bool empty() const {
        return read_.load(std::memory_order_relaxed) == write_.load(std::memory_order_acquire);
    }
    void clear() {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

  private:
    static constexpr size_t kMask = kCapacity - 1;
    T data_[kCapacity]{};
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

}  // namespace supple
