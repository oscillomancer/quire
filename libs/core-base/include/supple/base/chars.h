// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once

#include <charconv>

namespace supple {

// Locale-independent float/double parsing with std::from_chars' default
// general-format semantics. Returns the first unparsed character and errc;
// leaves value unchanged on invalid_argument or result_out_of_range.
std::from_chars_result fromChars(const char* first, const char* last, float& value) noexcept;
std::from_chars_result fromChars(const char* first, const char* last, double& value) noexcept;

}  // namespace supple
