// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Carries editor edits to the host as CLAP parameter events.
//
// The editor (main thread) records, per parameter, how many gestures began
// and ended and whether the value changed. The consumer -- process() or
// params.flush(), whichever the host calls next -- turns that into events:
//
//   BEGIN  when a gesture began that the host has not been told about,
//   VALUE  with the parameter's current value whenever anything changed,
//   END    once the editor's gesture is over.
//
// So the host always sees balanced gestures and always receives the final
// value, however the two threads interleave and however long a stopped host
// leaves edits undrained. Intermediate values that land between two drains
// collapse into one -- they would all have been stamped at frame 0 anyway.
// Storage is bounded by the parameter count: nothing can overflow. An output
// event the host refuses is retried on the next drain without reordering,
// and only it: a host that accepts one event per drain still gets every
// gesture through, one event at a time.
//
// The VALUE carries the parameter's current value, so if host automation
// lands on a parameter between a refused VALUE and its retry, the host is
// sent its own value back: the later write won, in the DSP and on screen.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/Definition.h"

namespace supple {

class EditBridge {
  public:
    enum class Kind { Begin, Value, End };

    // For `count` parameters, by table index.
    explicit EditBridge(size_t count) : count_(count) {
        for (size_t i = 0; i < count_; ++i) {
            begins_[i].store(0, std::memory_order_relaxed);
            ends_[i].store(0, std::memory_order_relaxed);
            dirty_[i].store(false, std::memory_order_relaxed);
        }
    }
    // A product with tables: its own.
    EditBridge() : EditBridge(productDefinition()->params().count()) {}

    // --- main thread --------------------------------------------------------
    void begin(size_t i) {
        if (open_[i]) return;
        open_[i] = true;
        begins_[i].fetch_add(1, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }
    // Call after the new value is in the ParamStore.
    void value(size_t i) {
        dirty_[i].store(true, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }
    void end(size_t i) {
        if (!open_[i]) return;
        open_[i] = false;
        ends_[i].fetch_add(1, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }
    bool open(size_t i) const { return open_[i]; }

    // --- consumer (audio thread, or main thread while inactive) -------------
    // emit(kind, index) pushes one event and returns false if the host
    // refused it. Returns false when anything is left for a retry.
    template <class Emit>
    bool drain(Emit&& emit) {
        if (!any_.exchange(false, std::memory_order_acq_rel)) return true;
        for (size_t i = 0; i < count_; ++i) {
            const uint32_t b = begins_[i].load(std::memory_order_acquire);
            const uint32_t e = ends_[i].load(std::memory_order_acquire);
            // Anything new since the last look means the value is owed again.
            if (dirty_[i].exchange(false, std::memory_order_acq_rel) || b != lookedBegins_[i] || e != lookedEnds_[i])
                owesValue_[i] = true;
            lookedBegins_[i] = b;
            lookedEnds_[i] = e;
            const bool editorOpen = b != e;
            if (!owesValue_[i] && b == toldBegins_[i] && !(hostOpen_[i] && !editorOpen)) continue;
            // Each event is acknowledged as it is accepted, so a refusal
            // repeats only what was refused, and every drain makes progress.
            if (!hostOpen_[i] && b != toldBegins_[i]) {
                if (!emit(Kind::Begin, i)) return retry();
                hostOpen_[i] = true;
            }
            toldBegins_[i] = b;
            if (owesValue_[i]) {
                if (!emit(Kind::Value, i)) return retry();
                owesValue_[i] = false;
            }
            if (hostOpen_[i] && !editorOpen) {
                if (!emit(Kind::End, i)) return retry();
                hostOpen_[i] = false;
            }
        }
        return true;
    }

    bool pending() const { return any_.load(std::memory_order_acquire); }

  private:
    // What is still owed is in the consumer's own state; this only makes sure
    // the next drain looks.
    bool retry() {
        any_.store(true, std::memory_order_release);
        return false;
    }

    template <class T>
    static std::unique_ptr<T[]> array(size_t n) {
        return std::make_unique<T[]>(n > 0 ? n : 1);  // value-initialized
    }

    size_t count_;
    // Written by the main thread only.
    std::unique_ptr<bool[]> open_ = array<bool>(count_);
    std::unique_ptr<std::atomic<uint32_t>[]> begins_ = array<std::atomic<uint32_t>>(count_);
    std::unique_ptr<std::atomic<uint32_t>[]> ends_ = array<std::atomic<uint32_t>>(count_);
    std::unique_ptr<std::atomic<bool>[]> dirty_ = array<std::atomic<bool>>(count_);
    std::atomic<bool> any_{false};
    // Consumer only.
    std::unique_ptr<uint32_t[]> lookedBegins_ = array<uint32_t>(count_);
    std::unique_ptr<uint32_t[]> lookedEnds_ = array<uint32_t>(count_);
    std::unique_ptr<uint32_t[]> toldBegins_ = array<uint32_t>(count_);
    std::unique_ptr<bool[]> owesValue_ = array<bool>(count_);
    std::unique_ptr<bool[]> hostOpen_ = array<bool>(count_);
};

}  // namespace supple
