// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Bounded VST3 text conversion, with no shared converter state.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace supple::host::unicode {

// Stop at NUL or the buffer boundary. Invalid UTF-16 becomes U+FFFD,
// without conversion exceptions. This is a main-thread allocating API.
inline std::string toUtf8(std::span<const char16_t> input) {
    std::string out;
    for (std::size_t i = 0; i < input.size() && input[i]; ++i) {
        std::uint32_t cp = input[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 1 < input.size() && input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (input[++i] - 0xdc00);
            } else {
                cp = 0xfffd;
            }
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            cp = 0xfffd;
        }
        const auto byte = [&](std::uint32_t value) { out.push_back(static_cast<char>(value)); };
        if (cp < 0x80) {
            byte(cp);
        } else if (cp < 0x800) {
            byte(0xc0 | (cp >> 6));
            byte(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            byte(0xe0 | (cp >> 12));
            byte(0x80 | ((cp >> 6) & 63));
            byte(0x80 | (cp & 63));
        } else {
            byte(0xf0 | (cp >> 18));
            byte(0x80 | ((cp >> 12) & 63));
            byte(0x80 | ((cp >> 6) & 63));
            byte(0x80 | (cp & 63));
        }
    }
    return out;
}

// NUL-terminate on success; false for malformed UTF-8 or insufficient room
// (including the terminator). No allocation; output is unspecified on failure.
inline bool toUtf16(std::string_view input, std::span<char16_t> out) noexcept {
    std::size_t used = 0;
    for (std::size_t i = 0; i < input.size();) {
        const auto first = static_cast<unsigned char>(input[i++]);
        std::uint32_t cp = first;
        unsigned remaining = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            cp = first & 31;
            remaining = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            cp = first & 15;
            remaining = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            cp = first & 7;
            remaining = 3;
            minimum = 0x10000;
        } else if (first >= 0x80) {
            return false;
        }
        if (remaining > input.size() - i) return false;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(input[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        const std::size_t units = cp >= 0x10000 ? 2 : 1;
        if (out.size() - used <= units) return false;
        if (units == 2) {
            cp -= 0x10000;
            out[used++] = static_cast<char16_t>(0xd800 + (cp >> 10));
            out[used++] = static_cast<char16_t>(0xdc00 + (cp & 1023));
        } else {
            out[used++] = static_cast<char16_t>(cp);
        }
    }
    if (used == out.size()) return false;
    out[used] = 0;
    return true;
}
}  // namespace supple::host::unicode
