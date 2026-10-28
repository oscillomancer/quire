// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Xlib defines None as a macro. The borrowed window header must also work
// after that definition, without requiring the X11 SDK in headless builds.
#define None 0L
#include "supple/base/native_window.h"
#undef None
static_assert(static_cast<std::uint8_t>(supple::NativeWindow::Kind::Unavailable) == 0);

#include <doctest/doctest.h>
#include <supple/base/closest.h>
#include <supple/base/units.h>
#include <ostream>

#include <supple/base/result.h>
#include <supple/product.h>

using supple::Error;
using supple::Result;

static Result<int> half(int x) {
    if (x % 2) return Error("test.odd", "value is odd", "/x", "pass an even number");
    return x / 2;
}

static supple::Status chain(int x) {
    SUPPLE_TRY(half(x));
    return {};
}

TEST_CASE("Result carries value or structured error") {
    auto a = half(4);
    REQUIRE(a);
    CHECK(*a == 2);
    auto b = half(3);
    REQUIRE(!b.ok());
    CHECK(b.error().code == "test.odd");
    CHECK(b.error().toString() == "test.odd at /x: value is odd Hint: pass an even number");
    CHECK(!chain(5).ok());
    CHECK(chain(6));
}

TEST_CASE("a failed REQUIRE(result) reports the error") {
    CHECK(doctest::toString(half(4)) == "ok");
    CHECK(doctest::toString(chain(6)) == "ok");
    CHECK(doctest::toString(half(3)) == "test.odd at /x: value is odd Hint: pass an even number");
    CHECK(doctest::toString(chain(5)) == "test.odd at /x: value is odd Hint: pass an even number");
}

TEST_CASE("product identity comes from the generated header") {
    CHECK(!supple::product::kName.empty());
    CHECK(supple::product::kVendorId == "com.suppleaudio");
}

TEST_CASE("base units and closest match") {
    using namespace supple;
    auto hz = units::parseQuantity("1.2 kHz", units::Unit::Hz);
    REQUIRE(hz);
    CHECK(*hz == 1200);
    CHECK(units::formatQuantity(1200, units::Unit::Hz) == "1.20 kHz");
    CHECK(units::formatQuantity(4, units::Unit::ratio) == "4.00:1");
    for (const char* text : {"1e308 kHz", "-1e308 kHz"}) {
        auto overflow = units::parseQuantity(text, units::Unit::Hz);
        REQUIRE_FALSE(overflow);
        CHECK(overflow.error().code == "schema.unit");
    }
    const std::string_view names[] = {"cutoff", "resonance"};
    CHECK(closestMatch("cuttoff", names) == "cutoff");
}

TEST_CASE("closest match preserves prefixes and ties when lengths differ") {
    using supple::closestMatch;
    const std::string_view names[] = {"cutoff", "cutoff_extended", "cut"};
    CHECK(closestMatch("cutoff_extende", names) == "cutoff_extended");
    CHECK(closestMatch("CUTOFF_extended_suffix", names) == "cutoff_extended");
    CHECK(closestMatch("", names) == "cut");
    CHECK(closestMatch(std::string(8192, 'z'), names).empty());
    const std::string_view tied[] = {"abc", "a"};
    CHECK(closestMatch("ab", tied) == "abc");
}
