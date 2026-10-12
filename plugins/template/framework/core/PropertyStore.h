// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Property values (core/Property.h), kept on the main thread and handed to the
// audio thread without a lock or an allocation there.
//
// The main thread owns a copy of every value. Setting one also publishes a
// fresh heap copy into that property's mailbox; the audio thread swaps it in
// at the start of its next block and returns the copy it replaced through a
// queue, which the main thread frees later. A value published twice before
// the audio thread looked is replaced in the mailbox, never seen. The audio
// thread only ever exchanges pointers, so a processor reads its values as
// string_views that stay valid until the next delivery.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/PropertyTable.h"
#include "core/Ring.h"

namespace supple {

template <class Table = PropertyTable>
class BasicPropertyStore {
  public:
    BasicPropertyStore() {
        for (size_t i = 0; i < Table::kCount; ++i) {
            main_[i] = Table::at(i).def;
            current_[i] = new std::string(main_[i]);
            pending_[i].store(nullptr, std::memory_order_relaxed);
        }
    }
    ~BasicPropertyStore() {
        collect();
        for (size_t i = 0; i < Table::kCount; ++i) {
            delete current_[i];
            delete pending_[i].exchange(nullptr, std::memory_order_acq_rel);
        }
    }
    BasicPropertyStore(const BasicPropertyStore&) = delete;
    BasicPropertyStore& operator=(const BasicPropertyStore&) = delete;

    // --- main thread -------------------------------------------------------------

    const std::string& get(size_t index) const { return main_[index]; }

    // False, and nothing changes, when the value is longer than the
    // property's limit. Setting the value it already has does nothing.
    bool set(size_t index, std::string_view value) {
        if (index >= Table::kCount || value.size() > Table::at(index).limit) return false;
        collect();
        if (main_[index] == value) return true;
        main_[index].assign(value.data(), value.size());
        delete pending_[index].exchange(new std::string(main_[index]), std::memory_order_acq_rel);
        version_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void resetToDefaults() {
        for (size_t i = 0; i < Table::kCount; ++i) set(i, Table::at(i).def);
    }

    // Changes with every value set, for an editor deciding whether to repaint.
    uint64_t version() const { return version_.load(std::memory_order_relaxed); }

    // Frees what the audio thread has finished with.
    void collect() {
        std::string* done = nullptr;
        while (retired_.pop(done)) delete done;
    }

    // --- audio thread, or the main thread while it is stopped -------------------

    // Takes every value published since the last call, calling
    // fn(index, value) for each.
    template <class Fn>
    void deliver(Fn&& fn) {
        for (size_t i = 0; i < Table::kCount; ++i) {
            if (!pending_[i].load(std::memory_order_relaxed) || !retired_.writable()) continue;
            std::string* fresh = pending_[i].exchange(nullptr, std::memory_order_acq_rel);
            if (!fresh) continue;
            retired_.push(current_[i]);
            current_[i] = fresh;
            fn(i, std::string_view(*fresh));
        }
    }

    // Audio-thread producer: a callback must let the main thread collect.
    bool needsCollection() const { return !retired_.writable(); }

    // Every value after prepare(), with the audio thread stopped. Drain all
    // mailboxes directly: a bounded retirement queue must not delay activation.
    template <class Fn>
    void deliverAll(Fn&& fn) {
        collect();
        for (size_t i = 0; i < Table::kCount; ++i) {
            if (auto* fresh = pending_[i].exchange(nullptr, std::memory_order_acq_rel)) {
                delete current_[i];
                current_[i] = fresh;
            }
            fn(i, std::string_view(*current_[i]));
        }
    }

  private:
    std::string main_[Table::kSlots];
    std::string* current_[Table::kSlots] = {};
    std::atomic<std::string*> pending_[Table::kSlots];
    Ring<std::string*, 6> retired_;
    std::atomic<uint64_t> version_{0};
};

using PropertyStore = BasicPropertyStore<>;

}  // namespace supple
