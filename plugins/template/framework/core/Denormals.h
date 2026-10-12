// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Flush-to-zero for the duration of one audio callback. A decaying filter or
// reverb tail that drifts into the subnormal range costs several times more
// per sample on x86 without it. Hosts often set the mode themselves, but not
// all do, and the thread is the host's: the previous mode is restored on exit.
#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define SUPPLE_FTZ_X86 1
#elif (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_MSC_VER)
#define SUPPLE_FTZ_ARM64 1
#endif

namespace supple {

class ScopedFlushDenormals {
  public:
    ScopedFlushDenormals() {
#if defined(SUPPLE_FTZ_X86)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);  // FTZ | DAZ
#elif defined(SUPPLE_FTZ_ARM64)
        uint64_t fpcr = 0;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        __asm__ __volatile__("msr fpcr, %0" ::"r"(fpcr | (uint64_t{1} << 24)));  // FZ
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(SUPPLE_FTZ_X86)
        _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(SUPPLE_FTZ_ARM64)
        __asm__ __volatile__("msr fpcr, %0" ::"r"(saved_));
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

  private:
    uint64_t saved_ = 0;
};

}  // namespace supple
