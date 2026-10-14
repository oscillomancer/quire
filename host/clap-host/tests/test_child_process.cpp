// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <array>
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
