// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A bounded filename component between the DSP artifact's prefix and suffix.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace supple::dsp_commands {
inline std::string artifactId(std::string_view id) {
    std::string readable;
    bool altered = id.size() > 64;
    std::uint64_t hash = 14695981039346656037ull;  // FNV-1a, stable across platforms
    for (const unsigned char c : id) {
        hash = (hash ^ c) * 1099511628211ull;
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        altered |= !safe;
        if (readable.size() < 64) readable += safe ? static_cast<char>(c) : '_';
    }
    if (readable.empty()) {
        readable = "_";
        altered = true;
    }
    if (altered) {
        readable += '-';
        constexpr char hex[] = "0123456789abcdef";
        for (int shift = 60; shift >= 0; shift -= 4) readable += hex[(hash >> shift) & 15];
    }
    return readable;
}
}  // namespace supple::dsp_commands
