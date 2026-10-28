// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "../src/media_files.h"

namespace fs = std::filesystem;
using namespace supple::engine;

TEST_CASE("media folder copy excludes its destination subtree and resolved aliases") {
    const auto root = fs::temp_directory_path() /
                      ("engine-media-copy-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        fs::path cwd = fs::current_path();
        ~Cleanup() {
            std::error_code ec;
            fs::current_path(cwd, ec);
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    const auto source = root / "media";
    const auto destination = source / "new" / "media";
    fs::create_directories(source / "rec");
    std::ofstream(source / "tone.wav") << "tone";
    std::ofstream(source / "rec" / "take.wav") << "take";
    auto target = destination;
    SUBCASE("existing destination") {
        fs::create_directories(destination);
        std::ofstream(destination / "existing.wav") << "keep";
    }
    SUBCASE("destination created during copying") {}
    SUBCASE("relative destination with no existing path components") {
        fs::current_path(source);
        target = fs::path("new") / "media";
    }
    SUBCASE("destination through a directory symlink") {
        std::error_code ec;
        fs::create_directory_symlink(source, root / "alias", ec);
        if (ec) {
            MESSAGE("directory symlink unavailable: " << ec.message());
            return;
        }
        target = root / "alias" / "new" / "media";
        fs::create_directories(target);
    }
    const auto copied = detail::copyFolder(source, target);
    REQUIRE(copied);
    CHECK(fs::exists(destination / "tone.wav"));
    CHECK(fs::exists(destination / "rec" / "take.wav"));
    CHECK_FALSE(fs::exists(destination / "new"));
    // A repeated Save As keeps identical files and still excludes the destination.
    REQUIRE(detail::copyFolder(source, target));
    CHECK_FALSE(fs::exists(destination / "new"));
    CHECK(fs::file_size(destination / "tone.wav") == 4);
    CHECK(fs::file_size(destination / "rec" / "take.wav") == 4);
    // Record in the nested project, then Save As back to the original folder.
    // An ancestor destination must receive new media, not exclude the source.
    std::ofstream(destination / "rec" / "new-take.wav") << "new take";
    REQUIRE(detail::copyFolder(target, source));
    REQUIRE(fs::exists(source / "rec" / "new-take.wav"));
    CHECK(fs::file_size(source / "rec" / "new-take.wav") == 8);
}
