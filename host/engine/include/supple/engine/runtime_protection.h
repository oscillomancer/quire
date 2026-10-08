// Model-thread-only Linux process controls. Injectable operations let tests
// exercise opt-in settings without changing the test process's memory locks.
#pragma once

#include <sys/resource.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace supple::engine {

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
};

}  // namespace supple::engine
