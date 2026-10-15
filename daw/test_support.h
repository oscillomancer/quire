// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstdlib>
#include <filesystem>
#if defined(_WIN32)
#include <process.h>
inline int testProcessId() { return _getpid(); }
#else
#include <unistd.h>
inline int testProcessId() { return getpid(); }
#endif
inline void testConfigHome(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
#if defined(_WIN32)
    _wputenv_s(L"APPDATA", directory.c_str());
    _wputenv_s(L"LOCALAPPDATA", directory.c_str());
#elif defined(__APPLE__)
    setenv("HOME", directory.c_str(), 1);
#else
    setenv("XDG_CONFIG_HOME", directory.c_str(), 1);
#endif
}
