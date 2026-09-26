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

// Writes a uniquely named temporary file in the same directory, fsyncs it,
// renames it over `path` and fsyncs the directory: readers see the old or the
// new file, never a partial one, concurrent writers never share a temp file,
// and a successful return survives a crash. Creates missing parent
// directories. Error code: "file.write".
Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
Status writeFileAtomic(const std::filesystem::path& path, std::string_view text);

// RFC 4648 base64 (standard alphabet, padded). Decoding rejects anything else
// (no whitespace, no URL alphabet): "base64.invalid".
std::string base64Encode(std::span<const std::uint8_t> bytes);
Result<std::vector<std::uint8_t>> base64Decode(std::string_view text);

}  // namespace supple
