// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/md5.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace supple {
namespace {

// K[i] = floor(|sin(i + 1)| * 2^32), computed once.
const std::array<std::uint32_t, 64>& constants() {
    static const std::array<std::uint32_t, 64> k = [] {
        std::array<std::uint32_t, 64> out{};
        for (std::size_t i = 0; i < 64; ++i)
            out[i] = static_cast<std::uint32_t>(
                std::floor(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
        return out;
    }();
    return k;
}

constexpr std::array<int, 16> kShift = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

Md5::Md5() : state_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u} {}

void Md5::block(const std::uint8_t* p) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = le32(p + 4 * i);
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    const auto& k = constants();
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        const int round = i / 16;
        if (round == 0) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (round == 1) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (round == 2) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t sum = a + f + k[static_cast<std::size_t>(i)] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + std::rotl(sum, kShift[static_cast<std::size_t>(round * 4 + i % 4)]);
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(std::span<const std::uint8_t> bytes) {
    if (finished_) return;
    bytes_ += bytes.size();
    std::size_t at = 0;
    if (buffered_) {
        const std::size_t take = std::min(bytes.size(), 64 - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes.data(), take);
        buffered_ += take;
        at = take;
        if (buffered_ < 64) return;
        block(buffer_.data());
        buffered_ = 0;
    }
    for (; at + 64 <= bytes.size(); at += 64) block(bytes.data() + at);
    if (at < bytes.size()) {
        buffered_ = bytes.size() - at;
        std::memcpy(buffer_.data(), bytes.data() + at, buffered_);
    }
}

void Md5::update(std::string_view text) {
    update(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::array<std::uint8_t, 16> Md5::finish() {
    std::array<std::uint8_t, 16> out{};
    if (!finished_) {
        const std::uint64_t bits = bytes_ * 8;
        std::uint8_t pad[72] = {0x80};
        const std::size_t padLength = (buffered_ < 56 ? 56 : 120) - buffered_;
        for (int i = 0; i < 8; ++i)
            pad[padLength + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(bits >> (8 * i));
        const std::uint64_t keep = bytes_;
        update(std::span<const std::uint8_t>(pad, padLength + 8));
        bytes_ = keep;
        finished_ = true;
    }
    for (int i = 0; i < 4; ++i)
        for (int b = 0; b < 4; ++b)
            out[static_cast<std::size_t>(4 * i + b)] =
                static_cast<std::uint8_t>(state_[static_cast<std::size_t>(i)] >> (8 * b));
    return out;
}

std::string md5Hex(std::string_view text) {
    Md5 md5;
    md5.update(text);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t byte : md5.finish()) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

}  // namespace supple
