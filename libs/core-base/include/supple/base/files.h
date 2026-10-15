// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Small whole-file helpers (main/I-O threads only).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
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
// directories. Error code: "file.write". On Windows a replace blocked by
// another program that holds the destination open retries for about a second
// before failing with an error that names the cause.
Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
Status writeFileAtomic(const std::filesystem::path& path, std::string_view text);

#ifdef _WIN32
// Windows only. Antivirus scanners, indexers, sync clients and viewers often
// hold a just-written file open without FILE_SHARE_DELETE, which fails a
// rename or replace for a moment. `attempt` performs one try and returns true
// on success; on failure it leaves the Win32 error in GetLastError(). While
// the failure is a sharing or lock violation (or access denied on a target
// that is not a directory) the attempt repeats with a short backoff, for about
// one second in all. Returns the last attempt's result; `winError` receives its
// error. Non-RT threads only: it sleeps.
bool retryWhileFileBusy(const std::filesystem::path& target, const std::function<bool()>& attempt,
                        unsigned long& winError);
// Whether `winError` (after retryWhileFileBusy gave up) means another program
// holds the file; `target` is the destination that was being replaced.
bool isFileBusyError(const std::filesystem::path& target, unsigned long winError);
// The structured "file.write" error for a replace that stayed blocked: it says
// another program has the file open and what to do about it.
Error fileBusyError(const std::filesystem::path& target, unsigned long winError);
#endif

// RFC 4648 base64 (standard alphabet, padded). Decoding rejects anything else
// (no whitespace, no URL alphabet): "base64.invalid".
std::string base64Encode(std::span<const std::uint8_t> bytes);
Result<std::vector<std::uint8_t>> base64Decode(std::string_view text);

}  // namespace supple
