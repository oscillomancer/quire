#pragma once

#include <memory>

#include "supple/base/result.h"
#include "supple/engine/runtime_protection.h"

namespace supple::engine::detail {

// Owned by one EngineApp in the host process; never used on an audio path.
class RuntimeProtection {
  public:
    explicit RuntimeProtection(std::shared_ptr<RuntimeProtectionOps> ops);
    ~RuntimeProtection();
    Status setMemoryLock(bool requested, bool running);
    void start();
    void stop();
    const RuntimeProtectionStatus& memoryLock() const { return memory_; }

  private:
    Status applyMemory();
    Status releaseMemory();
    std::shared_ptr<RuntimeProtectionOps> ops_;
    RuntimeProtectionStatus memory_;
};

}  // namespace supple::engine::detail
