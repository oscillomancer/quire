// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/json/json.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace {
bool hasNegativeZero(const supple::json::Value& value) {
    if (value.isDouble()) return value.asDouble() == 0 && std::signbit(value.asDouble());
    if (value.isArray()) {
        for (const auto& item : value.asArray())
            if (hasNegativeZero(item)) return true;
    }
    if (value.isObject()) {
        for (const auto& member : value.asObject())
            if (hasNegativeZero(member.value)) return true;
    }
    return false;
}
}  // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto value = supple::json::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!value) return 0;
    auto text = supple::json::write(*value);
    if (!text) __builtin_trap();
    auto again = supple::json::parse(*text);
    if (!again) __builtin_trap();
    if (!hasNegativeZero(*value) && !supple::json::identical(*value, *again)) __builtin_trap();
    if (supple::json::writeCanonical(*value) != supple::json::writeCanonical(*again))
        __builtin_trap();
    return 0;
}
