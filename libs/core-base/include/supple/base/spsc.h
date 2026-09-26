// Bounded single-producer / single-consumer ring for real-time channels.
//
// Contract (the same as the plugin template's Telemetry.h Ring, so plugin
// code and host code share one mental model):
//   - exactly one producer thread and one consumer thread;
//   - push() never overwrites: when full it returns false and the newest
//     item is dropped (the caller counts drops);
//   - peek()/consume() let the consumer look at an item before committing to
//     having taken it (so an item a downstream sink refuses can be retried);
//   - all operations are wait-free and allocation-free (SUPPLE_RT).
//
// Use it for *lossy* channels (telemetry, meters) and for bounded command
// queues whose producer handles `false`. State that must never be lost goes
// through coalescing slots instead (see ARCHITECTURE.md §3).
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>

#include "supple/base/rt.h"

namespace supple {

template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SpscRing capacity must be a power of two >= 2");
    // Trivially copyable: copying an item can never allocate, lock or throw,
    // which is what makes push/peek real-time safe.
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing items must be trivially copyable");

  public:
    static constexpr std::size_t kCapacity = Capacity;

    // Producer side ---------------------------------------------------------

    bool push(const T& item) SUPPLE_RT {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) return false;
        slots_[head & kMask] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Free slots as seen by the producer (exact for the producer; a lower
    // bound for anyone else).
    std::size_t free() const SUPPLE_RT {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return Capacity - (head - tail);
    }

    // Consumer side ---------------------------------------------------------

    bool peek(T& out) const SUPPLE_RT {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;
        out = slots_[tail & kMask];
        return true;
    }

    // Commits the item returned by the last successful peek(). Precondition:
    // that peek() returned true (calling it on an empty ring corrupts the ring).
    void consume() SUPPLE_RT {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        assert(tail != head_.load(std::memory_order_acquire) && "consume() without peek()");
        tail_.store(tail + 1, std::memory_order_release);
    }

    bool pop(T& out) SUPPLE_RT {
        if (!peek(out)) return false;
        consume();
        return true;
    }

    // Items available to the consumer (exact for the consumer).
    std::size_t size() const SUPPLE_RT {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return head - tail;
    }

    bool empty() const SUPPLE_RT { return size() == 0; }

  private:
    static constexpr std::size_t kMask = Capacity - 1;
    // The indices increase monotonically (wrapping is harmless: size_t
    // arithmetic is modular and Capacity divides 2^N).
    alignas(64) std::atomic<std::size_t> head_{0};  // written by the producer only
    alignas(64) std::atomic<std::size_t> tail_{0};  // written by the consumer only
    alignas(64) T slots_[Capacity]{};
};

}  // namespace supple
