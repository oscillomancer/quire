// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/environment.h>

#include <optional>
#include <string>

namespace {
constexpr const char* kFlag = "SUPPLE_TEST_ENVIRONMENT_FLAG";
int setFlag(const char* value) {
#ifdef _WIN32
    return ::_putenv_s(kFlag, value ? value : "");
#else
    return value ? ::setenv(kFlag, value, 1) : ::unsetenv(kFlag);
#endif
}
struct RestoreFlag {
    std::optional<std::string> previous;
    RestoreFlag() {
        if (const char* value = std::getenv(kFlag)) previous = value;
    }
    ~RestoreFlag() {
        if (previous)
            setFlag(previous->c_str());
        else
            setFlag(nullptr);
    }
};
}  // namespace

TEST_CASE("environment flags require exactly 1") {
    RestoreFlag restore;
    REQUIRE(setFlag(nullptr) == 0);
    CHECK_FALSE(supple::environmentFlagEnabled(kFlag));
    for (const char* value : {"", "0", "01", "10", "true", "1 ", " 1", "1\n"}) {
        CAPTURE(value);
        REQUIRE(setFlag(value) == 0);
        CHECK_FALSE(supple::environmentFlagEnabled(kFlag));
    }
    REQUIRE(setFlag("1") == 0);
    CHECK(supple::environmentFlagEnabled(kFlag));
}
