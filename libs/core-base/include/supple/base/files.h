// Small whole-file helpers (main/I-O threads only).
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"

namespace supple {

// Error codes: "file.open" (missing/unreadable), "file.read", "file.too_large".
Result<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path,
                                           std::size_t maxBytes = std::size_t(1) << 30);
Result<std::string> readTextFile(const std::filesystem::path& path,
                                 std::size_t maxBytes = std::size_t(1) << 30);

// Writes to "<path>.tmp" in the same directory, then renames over `path`, so
// readers see the old or the new file, never a partial one. Creates missing
// parent directories. Error code: "file.write".
Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
Status writeFileAtomic(const std::filesystem::path& path, std::string_view text);

// RFC 4648 base64 (standard alphabet, padded). Decoding rejects anything else
// (no whitespace, no URL alphabet): "base64.invalid".
std::string base64Encode(std::span<const std::uint8_t> bytes);
Result<std::vector<std::uint8_t>> base64Decode(std::string_view text);

}  // namespace supple
