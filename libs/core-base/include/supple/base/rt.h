// Real-time annotations.
//
// SUPPLE_RT marks a function as real-time: RealtimeSanitizer (the `rtsan`
// preset, -fsanitize=realtime) aborts if anything it calls allocates, locks,
// blocks or performs a system call. Mark every audio-thread entry point.
// rt::ScopedSanctionedSyscall scopes the one reviewed exception (bridged plugins).
#pragma once

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
// bounded wait of a bridged plugin (ADR-0013). Product code uses it there only.
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

}  // namespace supple::rt
