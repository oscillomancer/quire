// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The generated session's audio for GUI tests: written once per test process
// and hard-linked into each test's media folder, where generateSession finds
// it (writing eight 30 s WAVs took half of every fixture's setup).
#pragma once

#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "generate.h"

namespace supple::gui::test {

inline void linkGeneratedMedia(const std::filesystem::path& mediaDir) {
    namespace fs = std::filesystem;
    struct Shared {
        fs::path dir =
            fs::temp_directory_path() / ("supple-generated-media-" + std::to_string(::getpid()));
        Shared() {
            fs::remove_all(dir);
            REQUIRE(writeGeneratedMedia(dir));
        }
        ~Shared() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    };
    static const Shared shared;
    std::error_code ec;
    fs::create_directories(mediaDir, ec);
    for (const auto& entry : fs::directory_iterator(shared.dir)) {
        const fs::path to = mediaDir / entry.path().filename();
        if (fs::exists(to)) continue;
        fs::create_hard_link(entry.path(), to, ec);
        if (ec) fs::copy_file(entry.path(), to);  // another filesystem
    }
}

}  // namespace supple::gui::test
