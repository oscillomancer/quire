// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// SHA-256 (FIPS 180-4: state, 64-round compression, Merkle–Damgård padding).
//
// An original implementation, incremental so a large file is hashed as it is
// read and a recording is hashed as it is written. No allocation, no
// exceptions, no I/O except in sha256File (main/I-O threads only, like
// files.h). The digest is 32 raw bytes; the media model stores it as the
// "sha256:<lowercase hex>" string MediaSource::hash documents.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "supple/base/result.h"

namespace supple {

class Sha256 {
  public:
    Sha256();
    // Feeds bytes; call it as often as convenient.
    void update(std::span<const std::uint8_t> bytes);
    void update(std::string_view text);
    // The final digest. Ends the object: a further update() is ignored.
    std::array<std::uint8_t, 32> finish();
    // Bytes fed so far.
    std::uint64_t size() const { return bytes_; }

  private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t bytes_ = 0;
    bool finished_ = false;
};

// Lowercase hex, no prefix.
std::string sha256Hex(std::span<const std::uint8_t> bytes);
std::string sha256Hex(std::string_view text);
// The hex form of a digest Sha256::finish() returned.
std::string sha256Hex(const std::array<std::uint8_t, 32>& digest);
// "sha256:<lowercase hex>" of the file's bytes. Error codes as files.h:
// "file.open", "file.read".
Result<std::string> sha256File(const std::filesystem::path& path);

}  // namespace supple
