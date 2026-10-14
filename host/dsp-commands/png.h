// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A picture as a PNG file (8-bit RGBA, one zlib stream, filter 0), for
// dsp.preview: what an agent reads to see a plugin's editor. The system
// zlib does the compressing.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <zlib.h>

#include "supple/base/result.h"

namespace supple::dsp_commands::png {

namespace detail {

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = ~0u;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

inline void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}

inline void chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
                  std::span<const std::uint8_t> data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace detail

// The PNG of `rgba` (width * height * 4 bytes, rows from the top); empty
// when the sizes disagree.
inline std::vector<std::uint8_t> encode(int width, int height, std::span<const std::uint8_t> rgba) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return {};
    const auto w = static_cast<std::size_t>(width), h = static_cast<std::size_t>(height);
    if (rgba.size() != w * h * 4) return {};
    std::vector<std::uint8_t> raw;
    raw.reserve(h * (w * 4 + 1));
    for (std::size_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        const auto row = rgba.subspan(y * w * 4, w * 4);
        raw.insert(raw.end(), row.begin(), row.end());
    }
    uLongf packed = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(packed);
    if (compress2(z.data(), &packed, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        return {};
    z.resize(packed);
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> header;
    detail::put32(header, static_cast<std::uint32_t>(w));
    detail::put32(header, static_cast<std::uint32_t>(h));
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8 bits, RGBA, deflate, no filter, no interlace
    detail::chunk(out, "IHDR", header);
    detail::chunk(out, "IDAT", z);
    detail::chunk(out, "IEND", {});
    return out;
}

inline Status write(const std::filesystem::path& path, int width, int height,
                    std::span<const std::uint8_t> rgba) {
    const std::vector<std::uint8_t> bytes = encode(width, height, rgba);
    if (bytes.empty()) return Error("io.png", "the picture could not be encoded");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) return Error("io.write", "could not write " + path.string());
    return okStatus();
}

}  // namespace supple::dsp_commands::png
