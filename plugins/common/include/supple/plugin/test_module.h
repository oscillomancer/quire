// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Product-owned semantic stress tests, independent of template host/probe code.
#pragma once
#include <filesystem>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace supple::plugin::test {
inline void* open(const char* module) {
    std::filesystem::path binary = module;
#if defined(__APPLE__)
    // These tests consume our staged bundles, with one executable per bundle.
    std::error_code ec;
    if (std::filesystem::is_directory(binary, ec)) {
        for (const auto& file : std::filesystem::directory_iterator(binary / "Contents/MacOS", ec))
            if (file.is_regular_file(ec)) {
                binary = file.path();
                break;
            }
    }
#endif
#if defined(_WIN32)
    return static_cast<void*>(LoadLibraryW(binary.c_str()));
#else
    return ::dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
inline void* symbol(void* module, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    return ::dlsym(module, name);
#endif
}
inline void close(void* module) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(module));
#else
    ::dlclose(module);
#endif
}
inline const char* error() {
#if defined(_WIN32)
    return "LoadLibraryW failed";
#else
    return ::dlerror();
#endif
}
}  // namespace supple::plugin::test
