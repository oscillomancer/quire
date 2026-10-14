// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <supple/base/result.h>
#include <supple/json/value.h>
namespace supple::json {
Value mergePatch(const Value& target, const Value& patch);
Result<Value> createMergePatch(const Value& from, const Value& to);
struct Change {
    enum class Op { Add, Remove, Replace } op;
    std::string path;
    Value before, after;
};
std::vector<Change> diff(const Value& from, const Value& to);
}  // namespace supple::json
