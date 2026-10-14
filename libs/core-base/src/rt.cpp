// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/rt.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#else
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstddef>

namespace supple::rt {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(alignof(std::atomic<std::uint32_t>) >= 4);

void wake(std::atomic<std::uint32_t>& word) SUPPLE_RT {
    // ADR-0024: only the nonblocking syscall is exempt, never surrounding DSP.
    const ScopedSanctionedSyscall scope;
#if defined(_WIN32)
    ::WakeByAddressSingle(&word);
#elif defined(__APPLE__)
    ::os_sync_wake_by_address_any(&word, sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
#else
    ::syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#endif
}

void wait(std::atomic<std::uint32_t>& word, std::uint32_t seen) {
    while (word.load(std::memory_order_acquire) == seen) {
#if defined(_WIN32)
        ::WaitOnAddress(&word, &seen, sizeof(seen), INFINITE);
#elif defined(__APPLE__)
        ::os_sync_wait_on_address(&word, seen, sizeof(seen), OS_SYNC_WAIT_ON_ADDRESS_NONE);
#else
        ::syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0);
#endif
    }
}

bool waitUntil(std::atomic<std::uint32_t>& word, std::uint32_t seen,
               std::chrono::steady_clock::time_point deadline) {
    while (word.load(std::memory_order_acquire) == seen) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
#if defined(_WIN32)
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
        const auto timeout = static_cast<DWORD>(std::min<std::int64_t>(ms, INFINITE - 1));
        ::WaitOnAddress(&word, &seen, sizeof(seen), timeout);
#elif defined(__APPLE__)
        ::os_sync_wait_on_address_with_timeout(
            &word, seen, sizeof(seen), OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
            static_cast<std::uint64_t>(left.count()));
#else
        const timespec timeout{static_cast<time_t>(left.count() / 1'000'000'000),
                               static_cast<long>(left.count() % 1'000'000'000)};
        ::syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, seen, &timeout, nullptr, 0);
#endif
    }
    return true;
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
    void* low = nullptr;
    bool known = false;
#if defined(_WIN32)
    ULONG_PTR bottomLimit = 0, topLimit = 0;
    ::GetCurrentThreadStackLimits(&bottomLimit, &topLimit);
    low = reinterpret_cast<void*>(bottomLimit);
    known = topLimit > bottomLimit;
#elif defined(__APPLE__)
    const auto thread = ::pthread_self();
    const auto top = reinterpret_cast<std::uintptr_t>(::pthread_get_stackaddr_np(thread));
    const auto size = ::pthread_get_stacksize_np(thread);
    low = reinterpret_cast<void*>(top - size);
    known = top > size && size > 0;
#else
    pthread_attr_t attr;
    if (::pthread_getattr_np(::pthread_self(), &attr) != 0) return;
    std::size_t size = 0;
    known = ::pthread_attr_getstack(&attr, &low, &size) == 0;
    ::pthread_attr_destroy(&attr);
#endif
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    const auto bottom = reinterpret_cast<std::uintptr_t>(low);
    if (known && here > bottom && here - bottom >= 4 * kPrefaultBytes) touchStack();
}

}  // namespace supple::rt
