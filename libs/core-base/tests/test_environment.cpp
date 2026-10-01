#include <doctest/doctest.h>
#include <supple/base/environment.h>

#include <optional>
#include <string>

namespace {
constexpr const char* kFlag = "SUPPLE_TEST_ENVIRONMENT_FLAG";
struct RestoreFlag {
    std::optional<std::string> previous;
    RestoreFlag() {
        if (const char* value = std::getenv(kFlag)) previous = value;
    }
    ~RestoreFlag() {
        if (previous)
            setenv(kFlag, previous->c_str(), 1);
        else
            unsetenv(kFlag);
    }
};
}  // namespace

TEST_CASE("environment flags require exactly 1") {
    RestoreFlag restore;
    REQUIRE(unsetenv(kFlag) == 0);
    CHECK_FALSE(supple::environmentFlagEnabled(kFlag));
    for (const char* value : {"", "0", "01", "10", "true", "1 ", " 1", "1\n"}) {
        CAPTURE(value);
        REQUIRE(setenv(kFlag, value, 1) == 0);
        CHECK_FALSE(supple::environmentFlagEnabled(kFlag));
    }
    REQUIRE(setenv(kFlag, "1", 1) == 0);
    CHECK(supple::environmentFlagEnabled(kFlag));
}
