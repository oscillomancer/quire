// Bridge-local main-thread notification. Audio producers only publish atomics
// and wake a private futex; a parked relay makes the request pollable.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#include "supple/base/rt.h"

namespace supple::host {

class MainThreadWake {
  public:
    MainThreadWake();
    ~MainThreadWake();
    MainThreadWake(const MainThreadWake&) = delete;
    MainThreadWake& operator=(const MainThreadWake&) = delete;
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

// Round up so a sub-millisecond remainder does not busy-poll before the
// deadline. Saturate before narrowing: CLAP timer periods are uint32_t.
inline int timerTimeout(std::chrono::steady_clock::time_point next,
                        std::chrono::steady_clock::time_point now) {
    if (next <= now) return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(next - now).count();
    return static_cast<int>(std::min<std::int64_t>(ms, std::numeric_limits<int>::max()));
}

inline int nearestTimeout(int current, int next) {
    if (next < 0) return current;
    return current < 0 ? next : std::min(current, next);
}

}  // namespace supple::host
