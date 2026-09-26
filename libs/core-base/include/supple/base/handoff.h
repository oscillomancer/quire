// Wait-free hand-off of immutable objects from a control thread to the audio
// thread, with reclamation back on the control thread.
//
// This is the one pattern for "the main thread built something new (a render
// plan, a new patch's DSP state, a coefficient set); the audio thread should
// start using it at the next block; the old one must be freed, but never on
// the audio thread":
//
//   control thread                         audio thread
//   --------------                         ------------
//   handoff.publish(make_unique<Plan>(…))  Plan* plan = handoff.acquire();
//   handoff.collect()   // periodically    …use *plan for this block…
//
// - publish() never blocks. If the audio thread has not adopted the previous
//   publication yet, that one is replaced and freed immediately (the audio
//   thread never saw it).
// - acquire() is wait-free and allocation-free. It adopts the newest
//   publication only when it can hand the old object back (a bounded retire
//   ring); otherwise it keeps using the current object and tries again next
//   block — it never frees and never waits.
// - collect() frees retired objects on the control thread.
// - Once acquire() returns a new object, the previous one belongs to the
//   control thread and may be freed at any moment: the audio thread must not
//   touch it again (copy what it needs from the old object first, or keep its
//   own record — see the engine's list of processing plugin instances).
// - Generations let the control thread observe adoption (e.g. to know that a
//   render plan is live) without extra synchronization.
//
// One control thread and one audio thread per instance. The destructor must
// only run when neither thread uses the handoff any more.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "supple/base/rt.h"
#include "supple/base/spsc.h"

namespace supple {

template <typename T, std::size_t RetireCapacity = 16>
class RtHandoff {
    struct Node {
        std::unique_ptr<T> object;
        std::uint64_t generation;
    };

  public:
    RtHandoff() = default;
    RtHandoff(const RtHandoff&) = delete;
    RtHandoff& operator=(const RtHandoff&) = delete;

    ~RtHandoff() {
        delete pending_.load(std::memory_order_acquire);
        delete current_;
        collect();
    }

    // [control] Publishes `object` (may be null to publish "nothing").
    // Returns the publication's generation (1, 2, …).
    std::uint64_t publish(std::unique_ptr<T> object) {
        const std::uint64_t generation = ++published_;
        Node* node = new Node{std::move(object), generation};
        delete pending_.exchange(node, std::memory_order_acq_rel);
        collect();
        return generation;
    }

    // [control] Frees objects the audio thread has retired.
    void collect() {
        Node* node = nullptr;
        while (retired_.pop(node)) delete node;
    }

    // [control] Generation of the newest publication / of the object the audio
    // thread is currently using (0 before the first adoption).
    std::uint64_t publishedGeneration() const { return published_; }
    std::uint64_t adoptedGeneration() const { return adopted_.load(std::memory_order_acquire); }

    // [audio] Adopts the newest publication if possible and returns the
    // object in use (null before the first publication).
    T* acquire() SUPPLE_RT {
        if (pending_.load(std::memory_order_relaxed) != nullptr && retired_.free() > 0) {
            if (Node* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
                if (current_)
                    retired_.push(
                        current_);  // cannot fail: free() > 0 and we are the only producer
                current_ = next;
                adopted_.store(next->generation, std::memory_order_release);
            }
        }
        return current_ ? current_->object.get() : nullptr;
    }

    // [audio] The object in use, without adopting anything new.
    T* current() const SUPPLE_RT { return current_ ? current_->object.get() : nullptr; }

  private:
    std::atomic<Node*> pending_{nullptr};      // control → audio
    Node* current_ = nullptr;                  // owned by the audio thread
    SpscRing<Node*, RetireCapacity> retired_;  // audio → control
    std::uint64_t published_ = 0;              // control thread only
    std::atomic<std::uint64_t> adopted_{0};
};

}  // namespace supple
