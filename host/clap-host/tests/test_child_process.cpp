// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <array>
#include <fstream>
#include <iterator>
#include "supple/host/child_process.h"
using namespace supple::host;
TEST_CASE("child capture preserves arguments, exit code and bounded output") {
    const std::array<std::string, 5> args{"failure", "space value", "quote\"inside", "trailing\\",
                                          ""};
    auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
    REQUIRE(child);
    auto output =
        (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(10), 1024);
    REQUIRE(output);
    CHECK(output->text == "space value\nquote\"inside\ntrailing\\\n\n");
    CHECK(output->exitCode == 7);
    CHECK_FALSE(output->crashed);
}
TEST_CASE("child stderr is suppressed by default and inherited only when requested") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("supple-child-stderr-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } remove{file};
    for (const std::string inherit : {"0", "1"}) {
        for (const std::string capture : {"0", "1"}) {
            const std::array<std::string, 4> args{"stderr-parent", file.string(), inherit, capture};
            auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
            REQUIRE(child);
            auto output =
                (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(10), 32);
            REQUIRE(output);
            CHECK(output->exitCode == 0);
            CHECK(output->text.empty());
            std::ifstream input(file, std::ios::binary);
            REQUIRE(input);
            const std::string diagnostic((std::istreambuf_iterator<char>(input)), {});
            CHECK(diagnostic == (inherit == "1" ? "child stderr" : ""));
        }
    }
}
TEST_CASE("child timeout and output overflow are structured and clean up the child") {
    for (const std::string mode : {"hang", "flood"}) {
        const std::array<std::string, 1> args{mode};
        auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
        REQUIRE(child);
        auto output = (*child)->collect(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500), 64);
        REQUIRE_FALSE(output);
        CHECK(output.error().code == (mode == "hang" ? "child.timeout" : "child.output_limit"));
        CHECK_FALSE(output.error().path.empty());
        CHECK_FALSE(output.error().hint.empty());
    }
    const std::array<std::string, 0> args{};
    auto missing = ChildProcess::spawn("/nonexistent/supple-child", args);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "child.spawn");
}

#if defined(_WIN32)
#include <windows.h>
TEST_CASE("Windows child job terminates descendants retaining stdout") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("supple-child-descendant-" + std::to_string(GetCurrentProcessId()) + ".pid");
    const std::array<std::string, 2> args{"tree", file.string()};
    auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
    REQUIRE(child);
    auto output = (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(5), 64);
    REQUIRE_FALSE(output);
    CHECK(output.error().code == "child.timeout");
    DWORD pid = 0;
    std::ifstream input(file);
    REQUIRE(static_cast<bool>(input >> pid));
    input.close();
    HANDLE descendant = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    REQUIRE(descendant);
    struct Close {
        HANDLE value;
        ~Close() { CloseHandle(value); }
    } close{descendant};
    child->reset();
    CHECK(WaitForSingleObject(descendant, 5000) == WAIT_OBJECT_0);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
#endif

#if !defined(_WIN32)
TEST_CASE("child control inheritance preserves stdout capture with closed stdio") {
    const std::array<std::string, 1> args{"control-stdio"};
    auto child = ChildProcess::spawn(CHILD_FIXTURE_PATH, args);
    REQUIRE(child);
    auto output =
        (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(10), 32);
    REQUIRE(output);
    CHECK(output->exitCode == 0);
    CHECK_FALSE(output->crashed);
}
#endif
