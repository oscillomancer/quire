#pragma once
#include <supple/json/value.h>
namespace supple::json {
Value mergePatch(const Value& target, const Value& patch);
Value createMergePatch(const Value& from, const Value& to);
struct Change {
    enum class Op { Add, Remove, Replace } op;
    std::string path;
    Value before, after;
};
std::vector<Change> diff(const Value& from, const Value& to);
}  // namespace supple::json
