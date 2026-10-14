// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Entry-init failures for supervised scanning. Never loaded into the test host.
#include <clap/clap.h>
#include <signal.h>
#include <chrono>
#include <thread>
#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace {
bool CLAP_ABI init(const char*) {
    if (const char* log = std::getenv("SUPPLE_SCAN_FIXTURE_LOG")) {
        if (auto* file = std::fopen(log, "a")) {
            std::fprintf(file, "%d %ld\n", SCAN_MODE, static_cast<long>(::getpid()));
            std::fclose(file);
        }
    }
    if constexpr (SCAN_MODE == 1) {
#if defined(_WIN32)
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
        ::raise(SIGKILL);
#endif
    }
    if constexpr (SCAN_MODE == 2)
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(60));
    // A noisy but otherwise healthy plugin must not corrupt scan JSON.
    std::puts("plugin initialization log on stdout");
    return true;
}
void CLAP_ABI deinit() { std::puts("plugin unload log on stdout"); }
const char* const features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t descriptor = {CLAP_VERSION_INIT,
                                             "com.suppleaudio.test.scan",
                                             "Scan fixture",
                                             "Supple Audio",
                                             "",
                                             "",
                                             "",
                                             "1",
                                             "Supervised scanner fixture",
                                             features};
uint32_t CLAP_ABI count(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* CLAP_ABI describe(const clap_plugin_factory_t*, uint32_t i) {
    return i == 0 ? &descriptor : nullptr;
}
const clap_plugin_t* CLAP_ABI create(const clap_plugin_factory_t*, const clap_host_t*,
                                     const char*) {
    return nullptr;
}
const clap_plugin_factory_t factory = {count, describe, create};
const void* CLAP_ABI getFactory(const char*) { return &factory; }
}  // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, init, deinit,
                                                               getFactory};
