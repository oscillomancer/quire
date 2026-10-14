// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "history_sequence.h"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>

namespace {
struct Workdir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("supple-history-fuzz-" + std::to_string(::getpid()));
    ~Workdir() { std::filesystem::remove_all(path); }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 512) return 0;
    static Workdir workdir;
    supple::engine::AppOptions options;
    options.workdir = workdir.path;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto app = supple::engine::EngineApp::create(std::move(options));
    if (!app) std::abort();
    supple::cmd::Registry registry;
    supple::engine::registerEngineCommands(registry);
    supple::cmd::Dispatcher bus(registry, **app);
    auto result = supple::test::historySequence(**app, bus, std::span(data, size));
    if (!result) std::abort();
    return 0;
}
