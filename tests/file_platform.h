// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstdlib>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
namespace supple::test {
inline int processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}
inline int environment(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value ? value : "");
#else
    return value ? ::setenv(name, value, 1) : ::unsetenv(name);
#endif
}
}  // namespace supple::test
