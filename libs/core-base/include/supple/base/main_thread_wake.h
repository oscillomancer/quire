// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Coalesced main-thread notification. Audio producers only publish atomics
// and wake a private futex; a parked relay makes the request pollable.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "supple/base/rt.h"

namespace supple {

// Construct and destroy off audio. The relay must outlive every producer;
// stop/join producers before destruction. One main thread drains the fd.
class MainThreadWake {
  public:
    MainThreadWake();
    ~MainThreadWake();
    MainThreadWake(const MainThreadWake&) = delete;
    MainThreadWake& operator=(const MainThreadWake&) = delete;
    // Pollable notification fd; -1 if eventfd creation failed. Consume via drain().
    int fd() const { return fd_; }
    void notify() SUPPLE_RT;
    // Main thread, before servicing requests: acquire producers' publications
    // and allow the next request to notify. False if no relay event was ready.
    bool drain();

  private:
    friend struct MainThreadWakeTestAccess;
    enum class RelayStep { BeforeCapture, AfterCapture, BeforeWrite, Shutdown };
    using RelayHook = void (*)(void*, MainThreadWake&, RelayStep);
    MainThreadWake(RelayHook hook, void* context);
    // Cold-path scheduling hook for deterministic shutdown interleavings.
    RelayHook relayHook_ = nullptr;
    void* relayContext_ = nullptr;
    void observe(RelayStep step) {
        if (relayHook_) relayHook_(relayContext_, *this, step);
    }
    int fd_ = -1;
    std::atomic<bool> pending_{false}, quit_{false};
    std::atomic<std::uint32_t> sequence_{0};
    std::thread relay_;
    void relay();
};

}  // namespace supple
