// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// MD5 (RFC 1321's algorithm: four rounds of sixteen steps, sine-derived
// constants, Merkle–Damgård padding with a little-endian length).
//
// An original implementation for FLAC's STREAMINFO signature (the MD5 of the
// decoded samples), not for security. Incremental, no allocation, no I/O.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace supple {

class Md5 {
  public:
    Md5();
    void update(std::span<const std::uint8_t> bytes);
    void update(std::string_view text);
    // The 16-byte digest. Ends the object: a further update() is ignored.
    std::array<std::uint8_t, 16> finish();

  private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 4> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t bytes_ = 0;
    bool finished_ = false;
};

std::string md5Hex(std::string_view text);  // lowercase hex

}  // namespace supple
