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
    int fd_ = -1;
    std::atomic<bool> pending_{false}, quit_{false};
    std::atomic<std::uint32_t> sequence_{0};
    std::thread relay_;
    void relay();
};

}  // namespace supple
