// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Marks the functions the audio thread enters.
//
//   void process(Block& b) SUPPLE_REALTIME;
//
// In the rtsan preset (clang's RealtimeSanitizer) each marked function is a
// real-time context: anything beneath it that allocates, frees, locks a
// mutex, makes a blocking system call or touches a file aborts the test with
// a stack trace naming the call. That is the rule in AGENTS.md checked on
// every sample the tests render, rather than read for in review. In every
// other build the mark expands to nothing.
//
// The framework marks its own audio-thread entry points (Engine::process,
// flush and reset, and the CLAP process, flush and reset), so a product's
// processor is checked without marking anything itself.
#pragma once

#if defined(SUPPLE_RTSAN) && SUPPLE_RTSAN
#define SUPPLE_REALTIME [[clang::nonblocking]]
#else
#define SUPPLE_REALTIME
#endif
