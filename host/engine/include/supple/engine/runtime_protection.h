// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Model-thread-only Linux process controls. Injectable operations let tests
// exercise opt-in settings without changing the test process's memory locks.
#pragma once

#include <sys/resource.h>
#include <sys/types.h>
#include <cstddef>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace supple::engine {

// Permit shallow idle without deep-state exit delays. Fixed policy; no sysfs reads.
inline constexpr std::int32_t kCpuIdleLatencyUs = 20;

struct RuntimeProtectionError {
    std::string code, message, path, hint;
    std::int64_t errnoValue = 0;
};

struct RuntimeProtectionStatus {
    bool requested = false;
    bool active = false;
    std::optional<RuntimeProtectionError> error;
};

// Default operations call libc. Tests replace these before creating EngineApp.
struct RuntimeProtectionOps {
    RuntimeProtectionOps();
    std::function<int(int)> lockMemory;
    std::function<int()> unlockMemory;
    std::function<int(rlimit*)> memoryLimit;
    std::function<int(const char*, int)> openLatency;
    std::function<ssize_t(int, const void*, std::size_t)> writeLatency;
    std::function<int(int)> closeLatency;
};

}  // namespace supple::engine
