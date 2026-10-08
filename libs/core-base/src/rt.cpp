#include "supple/base/rt.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace supple::rt {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(alignof(std::atomic<std::uint32_t>) >= 4);

void wake(std::atomic<std::uint32_t>& word) SUPPLE_RT {
    // ADR-0024: only the nonblocking syscall is exempt, never surrounding DSP.
    const ScopedSanctionedSyscall scope;
    ::syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void wait(std::atomic<std::uint32_t>& word, std::uint32_t seen) {
    while (word.load(std::memory_order_acquire) == seen)
        ::syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0);
}

}  // namespace supple::rt
