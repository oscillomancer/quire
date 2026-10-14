// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/chars.h"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {
template <typename T>
void grammar() {
    for (const auto text : std::array<std::string_view, 6>{"", " 1", "+1", "--1", ".", "e4"}) {
        T value = 37;
        const auto result = supple::fromChars(text.data(), text.data() + text.size(), value);
        CHECK(result.ec == std::errc::invalid_argument);
        CHECK(result.ptr == text.data());
        CHECK(value == T(37));
    }
    for (const auto text :
         std::array<std::string_view, 3>{"1e1000 tail", "-1e1000 tail", "1e-1000 tail"}) {
        T value = 37;
        const auto result = supple::fromChars(text.data(), text.data() + text.size(), value);
        CHECK(result.ec == std::errc::result_out_of_range);
        CHECK(result.ptr == text.data() + text.find(' '));
        CHECK(value == T(37));
    }
    for (const auto text : std::array<std::string_view, 3>{"1e+", "1e", "1tail"}) {
        T value = 0;
        const auto result = supple::fromChars(text.data(), text.data() + text.size(), value);
        CHECK(result.ec == std::errc{});
        CHECK(result.ptr == text.data() + 1);
        CHECK(value == T(1));
    }
    for (const auto text : std::array<std::string_view, 2>{"-.5e+2", "-50.0"}) {
        T value = 0;
        const auto result = supple::fromChars(text.data(), text.data() + text.size(), value);
        CHECK(result.ec == std::errc{});
        CHECK(result.ptr == text.data() + text.size());
        CHECK(value == T(-50));
    }
    T value = 1;
    const std::string_view zero = "-0";
    CHECK(supple::fromChars(zero.data(), zero.data() + zero.size(), value).ec == std::errc{});
    CHECK(value == T(0));
    CHECK(std::signbit(value));
    const std::string_view inf = "-INFINITY";
    CHECK(supple::fromChars(inf.data(), inf.data() + inf.size(), value).ec == std::errc{});
    CHECK(std::isinf(value));
    CHECK(std::signbit(value));
    const std::string_view nan = "nan(payload)";
    const auto result = supple::fromChars(nan.data(), nan.data() + nan.size(), value);
    CHECK(result.ec == std::errc{});
    CHECK(result.ptr == nan.data() + nan.size());
    CHECK(std::isnan(value));
}

template <typename T>
void roundTrips() {
    using Bits = std::conditional_t<std::is_same_v<T, float>, std::uint32_t, std::uint64_t>;
    auto check = [](T value) {
        std::array<char, 64> text{};
        const auto written = std::to_chars(text.data(), text.data() + text.size(), value);
        REQUIRE(written.ec == std::errc{});
        T parsed = 0;
        const auto result = supple::fromChars(text.data(), written.ptr, parsed);
        REQUIRE(result.ec == std::errc{});
        CHECK(result.ptr == written.ptr);
        CHECK(std::bit_cast<Bits>(parsed) == std::bit_cast<Bits>(value));
    };
    for (T value : {T(0), T(-0.0), T(1), T(-1), std::numeric_limits<T>::min(),
                    std::numeric_limits<T>::max(), std::numeric_limits<T>::denorm_min()})
        check(value);
    std::uint64_t bits = 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < 2048; ++i) {
        bits = bits * 6364136223846793005ULL + 1442695040888963407ULL;
        const T value = std::bit_cast<T>(static_cast<Bits>(bits));
        if (std::isfinite(value)) check(value);
    }
}
}  // namespace

TEST_CASE("chars: float and double preserve from_chars grammar and error semantics") {
    grammar<float>();
    grammar<double>();
}

TEST_CASE("chars: decimal round trips preserve finite IEEE bit patterns") {
    roundTrips<float>();
    roundTrips<double>();
}

TEST_CASE("chars: halfway decimal values round to nearest even") {
    double d = 0;
    const std::string_view halfway = "1.00000000000000011102230246251565404236316680908203125";
    REQUIRE(supple::fromChars(halfway.data(), halfway.data() + halfway.size(), d).ec ==
            std::errc{});
    CHECK(d == 1.0);
    float f = 0;
    const std::string_view floatHalfway = "1.000000059604644775390625";
    REQUIRE(
        supple::fromChars(floatHalfway.data(), floatHalfway.data() + floatHalfway.size(), f).ec ==
        std::errc{});
    CHECK(f == 1.0f);
}
