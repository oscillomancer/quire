// Internal plug-in timer helpers; the shared notification relay is in core-base.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

#include "supple/base/main_thread_wake.h"

namespace supple::host {

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
