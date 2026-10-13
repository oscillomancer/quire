// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A running script's Shared and SharedArray members, on their way from the
// audio thread to the script's editor: a triple buffer of their bytes, laid
// end to end. The processor publishes whenever telemetry is due (while an
// editor is open, or a layer watches); the editor takes the newest and
// copies it into its own instance of the plugin, at the same offsets. Both
// sides are wait-free, and every member arrives as of one moment.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "supple/script_code.h"

namespace supple::suite {

class SharedChannel {
  public:
    explicit SharedChannel(std::vector<script::detail::SharedModel> regions)
        : regions_(std::move(regions)) {
        for (const auto& r : regions_) bytes_ += static_cast<size_t>(r.bytes);
        buffers_ = std::make_unique<unsigned char[]>(bytes_ * 3 + 1);
    }
    SharedChannel(const SharedChannel&) = delete;
    SharedChannel& operator=(const SharedChannel&) = delete;

    size_t bytes() const { return bytes_; }

    // Audio thread: the members of `self` (the plugin struct) as they are.
    void publish(const void* self) noexcept {
        gather(self, buffer(back_));
        back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex;
    }

    // Main thread: the newest bytes published, into `out` (bytes() long);
    // false, leaving `out` alone, when nothing arrived since the last take.
    bool take(std::vector<unsigned char>& out) {
        if (!(middle_.load(std::memory_order_acquire) & kFresh)) return false;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        out.assign(buffer(front_), buffer(front_) + bytes_);
        return true;
    }

    // The members of `self` set from bytes taken (bytes() long).
    void scatter(const std::vector<unsigned char>& from, void* self) const {
        if (from.size() != bytes_) return;
        size_t at = 0;
        for (const auto& r : regions_) {
            std::memcpy(static_cast<unsigned char*>(self) + r.offset, from.data() + at,
                        static_cast<size_t>(r.bytes));
            at += static_cast<size_t>(r.bytes);
        }
    }

  private:
    static constexpr int kIndex = 3, kFresh = 4;

    unsigned char* buffer(int k) const { return buffers_.get() + static_cast<size_t>(k) * bytes_; }
    void gather(const void* self, unsigned char* to) const noexcept {
        for (const auto& r : regions_) {
            std::memcpy(to, static_cast<const unsigned char*>(self) + r.offset,
                        static_cast<size_t>(r.bytes));
            to += r.bytes;
        }
    }

    std::vector<script::detail::SharedModel> regions_;
    size_t bytes_ = 0;
    std::unique_ptr<unsigned char[]> buffers_;
    int back_ = 0;   // the audio thread's
    int front_ = 1;  // the main thread's
    // The third, with kFresh while it holds what the main thread has not taken.
    std::atomic<int> middle_{2};
};

}  // namespace supple::suite
