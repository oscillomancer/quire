#include "supple/base/rt.h"

#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstddef>

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

namespace {
constexpr std::size_t kPrefaultBytes = 128 * 1024;

[[gnu::noinline]] void touchStack() {
    unsigned char stack[kPrefaultBytes];
    for (std::size_t end = kPrefaultBytes; end > 0; end -= 4096) stack[end - 1] = 0;  // top down
    // The array's address escapes, so the optimizer keeps its full size and
    // every store (a volatile local alone may be split into a few bytes).
    asm volatile("" : : "r"(stack) : "memory");
}
}  // namespace

void prefaultStack() {
    pthread_attr_t attr;
    if (::pthread_getattr_np(::pthread_self(), &attr) != 0) return;
    void* low = nullptr;
    std::size_t size = 0;
    const bool known = ::pthread_attr_getstack(&attr, &low, &size) == 0;
    ::pthread_attr_destroy(&attr);
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    const auto bottom = reinterpret_cast<std::uintptr_t>(low);
    if (known && here > bottom && here - bottom >= 4 * kPrefaultBytes) touchStack();
}

}  // namespace supple::rt
