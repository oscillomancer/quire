// Real-time annotations.
//
// SUPPLE_RT marks a function as real-time: RealtimeSanitizer (the `rtsan`
// preset, -fsanitize=realtime) aborts if anything it calls allocates, locks,
// blocks or performs a system call. Mark every audio-thread entry point.
// Reviewed syscall exceptions: background bridge IPC and live helper wake.
#pragma once

#include <atomic>
#include <cstdint>

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define SUPPLE_RT [[clang::nonblocking]]
#endif
#endif
#ifndef SUPPLE_RT
#define SUPPLE_RT
#endif

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#include <sanitizer/rtsan_interface.h>
#define SUPPLE_RTSAN_ENABLED 1
#endif
#endif

namespace supple::rt {

// RAII: disables RealtimeSanitizer checks in scope. Use only in tests (e.g. to
// build fixtures inside an RT-annotated harness) -- never in product code.
struct ScopedRtsanDisabler {
#ifdef SUPPLE_RTSAN_ENABLED
    ScopedRtsanDisabler() { __rtsan_disable(); }
    ~ScopedRtsanDisabler() { __rtsan_enable(); }
#endif
};

// RAII: the sanctioned system calls of an audio thread -- the futex wake and
// bounded wait of a background bridged plugin (ADR-0013), and the private
// futex wake inside rt::wake (ADR-0024). No callback waits are sanctioned.
struct ScopedSanctionedSyscall {
    ScopedSanctionedSyscall() {
#ifdef SUPPLE_RTSAN_ENABLED
        __rtsan_disable();
#endif
    }
    ~ScopedSanctionedSyscall() {
#ifdef SUPPLE_RTSAN_ENABLED
        __rtsan_enable();
#endif
    }
    ScopedSanctionedSyscall(const ScopedSanctionedSyscall&) = delete;
    ScopedSanctionedSyscall& operator=(const ScopedSanctionedSyscall&) = delete;
};

// Linux private futex notification. Publish the word with release before wake;
// wait acquires that publication. One waiter per word, lifetime through join.
void wake(std::atomic<std::uint32_t>& word) SUPPLE_RT;
// Idle helpers only: blocks until the word differs, including spurious wakes.
void wait(std::atomic<std::uint32_t>& word, std::uint32_t seen);
inline void relax() SUPPLE_RT {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

}  // namespace supple::rt
