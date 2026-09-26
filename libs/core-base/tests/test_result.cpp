#include <doctest/doctest.h>

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
    REQUIRE(a.ok());
    CHECK(*a == 2);
    auto b = half(3);
    REQUIRE(!b.ok());
    CHECK(b.error().code == "test.odd");
    CHECK(b.error().toString() == "test.odd at /x: value is odd Hint: pass an even number");
    CHECK(!chain(5).ok());
    CHECK(chain(6).ok());
}

TEST_CASE("product identity comes from the generated header") {
    CHECK(!supple::product::kName.empty());
    CHECK(supple::product::kVendorId == "com.suppleaudio");
}
