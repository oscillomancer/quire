#pragma once
#include <supple/base/result.h>
#include <supple/json/value.h>
namespace supple::json {
std::string pointerEscape(std::string_view token);
std::string pointerJoin(std::string_view base, std::string_view key);
std::string pointerJoin(std::string_view base, std::size_t index);
Result<std::vector<std::string>> pointerSplit(std::string_view pointer);
const Value* resolve(const Value& root, std::string_view pointer);
// A same-document URI reference holding a JSON Pointer fragment ("#",
// "#/$defs/name", percent-encoded as in RFC 6901 section 6), resolved against
// `root`. Null for anything else: another document, a malformed escape, a
// missing target.
const Value* resolveFragment(const Value& root, std::string_view reference);
}  // namespace supple::json
