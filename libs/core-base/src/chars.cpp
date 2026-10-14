// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/chars.h"

#include <fast_float/fast_float.h>

namespace supple {
namespace {
template <typename T>
std::from_chars_result parse(const char* first, const char* last, T& value) noexcept {
    // fast_float writes zero/infinity on range errors; std::from_chars keeps
    // the caller's value unchanged. Publish the parsed value only on success.
    T parsed{};
    const auto result = fast_float::from_chars(first, last, parsed);
    if (result.ec == std::errc{}) value = parsed;
    return {result.ptr, result.ec};
}
}  // namespace

std::from_chars_result fromChars(const char* first, const char* last, float& value) noexcept {
    return parse(first, last, value);
}

std::from_chars_result fromChars(const char* first, const char* last, double& value) noexcept {
    return parse(first, last, value);
}
}  // namespace supple
