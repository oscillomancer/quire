#include <doctest/doctest.h>
#include <supple/json/json.h>
#include <filesystem>
#include <fstream>
#include <limits>
using namespace supple::json;
TEST_CASE("parse and canonical output") {
    auto v = parse("{\"schema\":\"supple.test/1.0\",\"values\":[1,2.0,\"é\"]}");
    REQUIRE(v);
    auto out = writeCanonical(*v);
    CHECK(out.find("2.0") != std::string::npos);
    auto again = parse(out);
    REQUIRE(again);
    CHECK(identical(*again, *v));
    CHECK(writeCanonical(*again) == out);
}
TEST_CASE("parser errors") {
    for (auto [input, code] : {std::pair{"[1,]", "trailing_comma"},
                               {"{a:1}", "expected_key"},
                               {"{\"a\":1,\"a\":2}", "duplicate_key"},
                               {"NaN", "nan_or_infinity"},
                               {"//x", "comment"},
                               {"1e999", "number_out_of_range"}}) {
        auto r = parse(input);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == std::string("json.parse.") + code);
        CHECK(r.error().loc.has_value());
        CHECK(r.error().message.find('^') != std::string::npos);
    }
}
TEST_CASE("pointer merge and diff") {
    auto a = parse("{\"a/b\":1,\"x\":2}");
    REQUIRE(a);
    CHECK(resolve(*a, "/a~1b")->asInt() == 1);
    auto patch = parse("{\"x\":null,\"y\":3}");
    REQUIRE(patch);
    auto b = mergePatch(*a, *patch);
    CHECK(b.find("x") == nullptr);
    CHECK(b.find("y")->asInt() == 3);
    auto created = createMergePatch(*a, b);
    REQUIRE(created);
    CHECK(mergePatch(*a, *created) == b);
    CHECK(diff(*a, b).size() == 2);
}
TEST_CASE("all parse diagnostics") {
    struct Case {
        const char* text;
        const char* code;
    };
    const Case cases[] = {{"", "unexpected_end"},
                          {"?", "unexpected_char"},
                          {"{\"a\":1 \"b\":2}", "expected_comma_or_end"},
                          {"{\"a\" 1}", "expected_colon"},
                          {"{a:1}", "expected_key"},
                          {"[1,]", "trailing_comma"},
                          {"01", "invalid_number"},
                          {"1e999", "number_out_of_range"},
                          {"\"\\q\"", "invalid_escape"},
                          {"\"\\uD800\"", "invalid_unicode"},
                          {"\"\xff\"", "invalid_utf8"},
                          {"\"\n\"", "control_char_in_string"},
                          {"{\"a\":1,\"a\":2}", "duplicate_key"},
                          {"[[0]]", "too_deep"},
                          {"true ", "too_large"},
                          {"true false", "trailing_content"},
                          {"\xef\xbb\xbf{}", "bom"},
                          {"'x'", "single_quotes"},
                          {"// x", "comment"},
                          {"NaN", "nan_or_infinity"}};
    for (const auto& c : cases) {
        ParseOptions options;
        if (std::string(c.code) == "too_deep") options.maxDepth = 1;
        if (std::string(c.code) == "too_large") options.maxBytes = 3;
        auto r = parse(c.text, options);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == std::string("json.parse.") + c.code);
        CHECK(r.error().loc.has_value());
        CHECK(!r.error().hint.empty());
    }
}
TEST_CASE("RFC pointer and merge examples") {
    auto doc = parse("{\"foo\":[\"bar\",\"baz\"],\"\":0,\"a/b\":1,\"m~n\":2}");
    REQUIRE(doc);
    CHECK(resolve(*doc, "") == &*doc);
    CHECK(resolve(*doc, "/foo/0")->asString() == "bar");
    CHECK(resolve(*doc, "/")->asInt() == 0);
    CHECK(resolve(*doc, "/a~1b")->asInt() == 1);
    CHECK(resolve(*doc, "/m~0n")->asInt() == 2);
    CHECK(resolve(*doc, "/foo/01") == nullptr);
    CHECK_FALSE(pointerSplit("/bad~3"));
    auto target = parse("{\"a\":\"b\",\"c\":{\"d\":\"e\",\"f\":\"g\"}}");
    auto patch = parse("{\"a\":\"z\",\"c\":{\"f\":null}}");
    REQUIRE(target);
    REQUIRE(patch);
    CHECK(writeCompact(mergePatch(*target, *patch)) == "{\"a\":\"z\",\"c\":{\"d\":\"e\"}}");
}
TEST_CASE("writer errors and scalar array layout") {
    auto w = write(Value(std::numeric_limits<double>::infinity()));
    CHECK_FALSE(w);
    CHECK(w.error().code == "json.write.not_finite");
    auto a = parse("[1,2,3]");
    REQUIRE(a);
    CHECK(writeCanonical(*a) == "[1, 2, 3]\n");
    WriteOptions options;
    options.maxLineWidth = 5;
    auto longLine = write(*a, options);
    REQUIRE(longLine);
    CHECK(longLine->find("\n") != std::string::npos);
}
TEST_CASE("RFC 8259 corpus") {
    namespace fs = std::filesystem;
    auto root = fs::path(__FILE__).parent_path() / "corpus";
    for (auto kind : {"accept", "reject"})
        for (const auto& entry : fs::directory_iterator(root / kind)) {
            std::ifstream in(entry.path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), {});
            auto r = parse(text);
            INFO(entry.path().string());
            if (std::string(kind) == "accept") {
                REQUIRE(r);
                auto encoded = writeCanonical(*r);
                auto again = parse(encoded);
                REQUIRE(again);
                CHECK(writeCanonical(*again) == encoded);
            } else
                CHECK_FALSE(r);
        }
}
TEST_CASE("merge patch property for document values") {
    std::uint32_t state = 0x12345678;
    auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (int i = 0; i < 1000; ++i) {
        Value a = Value::object(), b = Value::object();
        for (int j = 0; j < 5; ++j) {
            auto key = std::to_string(j);
            if (next() % 3) a[key] = static_cast<int>(next() % 100);
            if (next() % 3) b[key] = static_cast<int>(next() % 100);
        }
        auto patch = createMergePatch(a, b);
        REQUIRE(patch);
        CHECK(mergePatch(a, *patch) == b);
    }
}
TEST_CASE("numeric equality preserves large integer precision") {
    CHECK(Value(1) == Value(1.0));
    CHECK_FALSE(Value(9007199254740993LL) == Value(9007199254740992.0));
}
TEST_CASE("strict identity and saturated integers") {
    Value ordered = Value::object();
    ordered["a"] = 1;
    ordered["b"] = 1.0;
    Value reversed = Value::object();
    reversed["b"] = 1.0;
    reversed["a"] = 1;
    CHECK(ordered == reversed);
    CHECK_FALSE(identical(ordered, reversed));
    CHECK(Value(1) == Value(1.0));
    CHECK_FALSE(identical(Value(1), Value(1.0)));
    CHECK_FALSE(identical(Value(-0.0), Value(0.0)));
    CHECK(Value(0x1p63).asInt() == std::numeric_limits<std::int64_t>::max());
    CHECK(Value(-0x1p64).asInt() == std::numeric_limits<std::int64_t>::min());
    CHECK(Value(1.5).asInt() == 0);
}
TEST_CASE("merge patch reports unrepresentable null") {
    auto from = parse("{\"a\":1}");
    auto to = parse("{\"a\":null}");
    REQUIRE(from);
    REQUIRE(to);
    auto patch = createMergePatch(*from, *to);
    REQUIRE_FALSE(patch);
    CHECK(patch.error().code == "json.merge.unrepresentable_null");
    CHECK(patch.error().path == "/a");
    auto arrayFrom = parse("{\"a\":[]}");
    auto arrayTo = parse("{\"a\":[null]}");
    REQUIRE(arrayFrom);
    REQUIRE(arrayTo);
    auto valid = createMergePatch(*arrayFrom, *arrayTo);
    REQUIRE(valid);
    CHECK(mergePatch(*arrayFrom, *valid) == *arrayTo);
}
TEST_CASE("release writer replaces non-finite numbers with null") {
    Value nested = Value::object();
    nested["bad"] = std::numeric_limits<double>::infinity();
    auto safe = write(nested);
    REQUIRE_FALSE(safe);
    CHECK(safe.error().code == "json.write.not_finite");
#ifdef NDEBUG
    CHECK(writeCompact(nested) == "{\"bad\":null}");
    CHECK(writeCanonical(nested) == "{\n  \"bad\": null\n}\n");
#endif
}
