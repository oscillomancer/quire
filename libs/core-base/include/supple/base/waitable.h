// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once

#include <cstdint>
#include <span>

namespace supple {

// Borrowed native readiness source. Close only after all watchers stop.
// POSIX: descriptor; Windows: manual-reset event HANDLE. Never waited on RT.
class Waitable {
  public:
    Waitable() = default;
    Waitable(int fd) : value_(fd) {}
    static Waitable handle(void* handle) {
        Waitable result;
        result.value_ = reinterpret_cast<std::intptr_t>(handle);
        result.handle_ = true;
        return result;
    }
    bool valid() const { return handle_ ? value_ != 0 && value_ != -1 : value_ >= 0; }
    int fd() const { return handle_ ? -1 : static_cast<int>(value_); }
    void* nativeHandle() const { return handle_ ? reinterpret_cast<void*>(value_) : nullptr; }
    bool operator==(const Waitable&) const = default;

  private:
    std::intptr_t value_ = -1;
    bool handle_ = false;
};

inline constexpr short kWaitRead = 0x001, kWaitWrite = 0x004, kWaitError = 0x008,
                       kWaitHangup = 0x010, kWaitInvalid = 0x020;
struct WaitItem {
    Waitable source;
    short events = kWaitRead;
    short ready = 0;
};
// Returns the number of ready entries (including duplicate sources), 0 at the
// deadline, -1 on error (errno). A -1 timeout waits indefinitely.
// Windows accepts at most 64 event handles, reports invalid sources as ready.
int waitFor(std::span<WaitItem> items, int timeoutMs);

// Non-RT, level-triggered coalescing event. Linux eventfd, macOS pipe,
// Windows manual-reset event. notify is also signal-safe on POSIX.
class WakeEvent {
  public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;
    Waitable waitable() const { return source_; }
    void notify() const;
    bool drain();
    // POSIX signal handlers borrow the write end (eventfd on Linux).
    int writeFd() const { return writeFd_; }

  private:
    Waitable source_;
    int writeFd_ = -1;
};
}  // namespace supple
