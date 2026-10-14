// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <span>
#include <string>
#include <string_view>
namespace supple {
std::string closestMatch(std::string_view input, std::span<const std::string_view> choices);
}
