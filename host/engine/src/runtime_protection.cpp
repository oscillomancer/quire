#include "runtime_protection.h"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>

namespace supple::engine {

RuntimeProtectionOps::RuntimeProtectionOps()
    : lockMemory([](int flags) { return ::mlockall(flags); }),
      unlockMemory([] { return ::munlockall(); }),
      memoryLimit([](rlimit* limit) { return ::getrlimit(RLIMIT_MEMLOCK, limit); }) {}

namespace detail {
namespace {
Status failure(RuntimeProtectionStatus& state, int value, const char* code, const char* operation,
               const char* path, std::string hint) {
    RuntimeProtectionError error{code,
                                 std::string(operation) + ": " + std::strerror(value) + " (errno " +
                                     std::to_string(value) + ")",
                                 path, std::move(hint), value};
    state.error = error;
    return Error(error.code, error.message, error.path, error.hint);
}
}  // namespace

RuntimeProtection::RuntimeProtection(std::shared_ptr<RuntimeProtectionOps> ops)
    : ops_(ops ? std::move(ops) : std::make_shared<RuntimeProtectionOps>()) {}

RuntimeProtection::~RuntimeProtection() { stop(); }

Status RuntimeProtection::setMemoryLock(bool requested, bool running) {
    memory_.requested = requested;
    if (!requested) {
        if (memory_.active) return releaseMemory();
        memory_.error.reset();
        return {};
    }
    return running ? applyMemory() : Status{};
}

Status RuntimeProtection::applyMemory() {
    if (memory_.active) return {};
    if (ops_->lockMemory(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) == 0) {
        memory_.active = true;
        memory_.error.reset();
        return {};
    }
    const int value = errno;
    std::string hint = "disable memory locking, or retry after resolving the OS error";
    if (value == ENOMEM || value == EPERM) {
        rlimit limit{};
        const std::string allowance = ops_->memoryLimit(&limit) != 0 ? "unavailable"
                                      : limit.rlim_cur == RLIM_INFINITY
                                          ? "unlimited"
                                          : std::to_string(limit.rlim_cur) + " bytes";
        hint =
            "disable memory locking or reduce mapped memory; ask an administrator to review "
            "the account's memlock allowance (currently " +
            allowance + ")";
    } else if (value == EINVAL) {
        hint = "disable memory locking; the kernel does not support the requested flags";
    }
    return failure(memory_, value, "engine.memory_lock", "mlockall", "/lock_memory",
                   std::move(hint));
}

Status RuntimeProtection::releaseMemory() {
    if (!memory_.active) return {};
    if (ops_->unlockMemory() != 0) {
        const int value = errno;
        return failure(memory_, value, "engine.memory_unlock", "munlockall", "/lock_memory",
                       "retry disabling memory locking; process exit also releases all locks");
    }
    memory_.active = false;
    memory_.error.reset();
    return {};
}

void RuntimeProtection::start() {
    if (memory_.requested) (void)applyMemory();
}

void RuntimeProtection::stop() { (void)releaseMemory(); }

}  // namespace detail
}  // namespace supple::engine
