// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Names and scheduling for the threads the host owns (engine.md §2.2,
// ADR-0026, ADR-0034). Setup only: never call from an audio path.
#pragma once

#include <cstdint>
#include <string_view>
#include <thread>

#include "supple/base/result.h"

namespace supple {

// Names the calling thread for `ps -L`, `top -H`, debuggers and profilers.
// Linux keeps at most 15 bytes; a longer name is cut there.
void nameThread(std::string_view name);

// A thread's scheduling class. `None`: there is no such thread, or its
// scheduling could not be read.
struct ThreadPolicy {
    enum class Kind : std::uint8_t { None, Normal, Fifo, RoundRobin, High };
    Kind kind = Kind::None;
    int priority = 0;  // 1..99 for Fifo and RoundRobin; 0 otherwise
    bool realtime() const { return kind == Kind::Fifo || kind == Kind::RoundRobin; }
    bool operator==(const ThreadPolicy&) const = default;
};
// "none", "normal", "fifo", "rr" or "high".
std::string_view policyName(ThreadPolicy::Kind kind);
// What `thread` runs with: SCHED_OTHER, BATCH and IDLE are Normal.
ThreadPolicy threadPolicy(std::thread::native_handle_type thread);
// Gives `thread` the policy (Normal: SCHED_OTHER, its nice value unchanged)
// and verifies it. Fifo and RoundRobin are refused (`thread.rttime`) while
// RLIMIT_RTTIME is finite: the kernel kills a real-time thread that runs that
// long without blocking. Permission failures are `thread.policy`.
// High: Windows thread priority, macOS self-applied QoS; not real-time.
Status setThreadPolicy(std::thread::native_handle_type thread, ThreadPolicy policy);

// Setup only: CPUs available to this process, with hardware-count fallback.
unsigned availableCpus();
// Like availableCpus, but Linux reports 0 if affinity could not be read.
unsigned allowedCpuCount();
// Linux process RT limit; other platforms leave the requested value unchanged.
int realtimePriorityLimit(int requested);
}  // namespace supple
