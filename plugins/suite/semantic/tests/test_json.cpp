// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <string>

#include "supple/suite/json.h"

using namespace supple::suite::json;

namespace {
Value parsed(std::string_view text) {
    Value v;
    Error e;
    REQUIRE_MESSAGE(parse(text, v, e), e.message);
    return v;
}
std::string errorCode(std::string_view text) {
    Value v;
    Error e;
    CHECK_FALSE(parse(text, v, e));
    CHECK_FALSE(e.hint.empty());
    return e.code;
}
}  // namespace

TEST_CASE("canonical text reads back as the same value, and writes the same bytes") {
    const char* text = R"({"schema": "supple.x-patch/1.0", "n": 3, "gain": -3.5, "tiny": 1e-07,
        "list": [1, 2.5, "a"], "empty": {}, "none": [], "nested": {"k": [{"a": true}, null]},
        "text": "tab\tquote\" é"})";
    const Value v = parsed(text);
    const std::string once = writeCanonical(v);
    const Value again = parsed(once);
    CHECK(again == v);
    CHECK(writeCanonical(again) == once);
    CHECK(once.find("\"n\": 3,") != std::string::npos);  // an integer stays one
    CHECK(once.find("\"gain\": -3.5,") != std::string::npos);
    CHECK(once.find("\"list\": [1, 2.5, \"a\"],") != std::string::npos);
    CHECK(once.back() == '\n');
    CHECK(writeCompact(parsed("[1, {\"a\": 2}]")) == "[1,{\"a\":2}]");
}

TEST_CASE("doubles are the shortest text that reads back exactly") {
    CHECK(formatDouble(1.0) == "1.0");
    CHECK(formatDouble(-0.0) == "0.0");
    CHECK(formatDouble(0.1) == "0.1");
    CHECK(formatDouble(1e21) == "1e+21");
    CHECK(parsed(formatDouble(0.30000000000000004)).asDouble() == 0.30000000000000004);
}

TEST_CASE("wide objects preserve insertion order and reject decoded duplicate keys") {
    std::string compact = "{", canonical = "{\n";
    for (int i = 4095; i >= 0; --i) {
        const std::string member = "\"key" + std::to_string(i) + "\"";
        compact += (i == 4095 ? "" : ",") + member + ":" + std::to_string(i);
        canonical += "  " + member + ": " + std::to_string(i) + (i == 0 ? "\n" : ",\n");
    }
    compact += "}";
    canonical += "}\n";
    const Value wide = parsed(compact);
    CHECK(wide.size() == 4096);
    CHECK(writeCompact(wide) == compact);
    CHECK(writeCanonical(wide) == canonical);
    CHECK(writeCanonical(parsed(canonical)) == canonical);
    compact.pop_back();
    // The first key repeated through a JSON escape, before even a colon:
    // duplicate-key errors take precedence over missing-colon errors.
    compact += ",\"\\u006bey4095\"}";
    Value out(42);
    Error e;
    CHECK_FALSE(parse(compact, out, e));
    CHECK(e.code == "json.parse.duplicate_key");
    CHECK(e.hint == "keep one of them");
    CHECK(e.line == 1);
    CHECK(e.column == static_cast<int>(compact.size()));
    CHECK(out == Value(42));
    CHECK(writeCompact(parsed(R"({"key": {"key": 1}, "next": {"key": 2}})")) ==
          R"({"key":{"key":1},"next":{"key":2}})");
}

TEST_CASE("the mistakes people and models make get their own errors") {
    CHECK(errorCode("{\"a\": 1,}") == "json.parse.trailing_comma");
    CHECK(errorCode("[1, 2,]") == "json.parse.trailing_comma");
    CHECK(errorCode("{'a': 1}") == "json.parse.single_quotes");
    CHECK(errorCode("{\"a\": 1 // why\n}") == "json.parse.expected_comma_or_end");
    CHECK(errorCode("// a comment\n{}") == "json.parse.comment");
    CHECK(errorCode("{\"a\": NaN}") == "json.parse.nan_or_infinity");
    CHECK(errorCode("{\"a\": 1, \"a\": 2}") == "json.parse.duplicate_key");
    CHECK(errorCode("{\"a\" 1}") == "json.parse.expected_colon");
    CHECK(errorCode("[01]") == "json.parse.invalid_number");
    CHECK(errorCode("\"\xff\"") == "json.parse.invalid_utf8");
    CHECK(errorCode("\"unfinished\\") == "json.parse.unexpected_end");
    CHECK(errorCode("{} {}") == "json.parse.trailing_content");
    Value v;
    Error e;
    CHECK_FALSE(parse("{\n  \"cutoff\": 1200 \"res\": 0.3\n}", v, e));
    CHECK(e.line == 2);
    CHECK(e.column == 18);
    CHECK(e.message.find("^") != std::string::npos);
    CHECK_FALSE(e.hint.empty());
}

TEST_CASE("merge patches follow RFC 7396") {
    const Value target = parsed(R"({"a": "b", "c": {"d": "e", "f": "g"}})");
    const Value patch = parsed(R"({"a": "z", "c": {"f": null}})");
    CHECK(mergePatch(target, patch) == parsed(R"({"a": "z", "c": {"d": "e"}})"));
    CHECK(mergePatch(parsed("[1]"), parsed(R"({"a": 1})")) == parsed(R"({"a": 1})"));
    CHECK(pointerJoin("/params", "a/b~c") == "/params/a~1b~0c");
}
