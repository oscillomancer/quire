// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace supple::engine::detail::files {
inline int processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}
// Publish a complete temporary file without replacing an existing target.
// The caller removes any remaining temp name afterwards (Windows consumes it).
inline int publishNew(const std::filesystem::path& temp, const std::filesystem::path& target) {
#if defined(_WIN32)
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) return 0;
    const DWORD error = GetLastError();
    switch (error) {
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            errno = EEXIST;
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            errno = ENOENT;
            break;
        case ERROR_DISK_FULL:
            errno = ENOSPC;
            break;
        case ERROR_NOT_SAME_DEVICE:
            errno = EXDEV;
            break;
        case ERROR_ACCESS_DENIED:
            errno = EACCES;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
#else
    return ::link(temp.c_str(), target.c_str());
#endif
}
// Nonserialized cache identity, follows symlinks. No file reads on audio threads.
inline std::optional<std::string> identity(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool found = GetFileInformationByHandle(file, &info) != 0;
    CloseHandle(file);
    if (!found) return std::nullopt;
    const auto combine = [](DWORD high, DWORD low) { return (std::uint64_t{high} << 32) | low; };
    return "file:" + std::to_string(info.dwVolumeSerialNumber) + ":" +
           std::to_string(combine(info.nFileIndexHigh, info.nFileIndexLow)) + ":" +
           std::to_string(combine(info.nFileSizeHigh, info.nFileSizeLow)) + ":" +
           std::to_string(
               combine(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime));
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) return std::nullopt;
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec;
#else
    const auto modified = info.st_mtim;
#endif
    return "file:" + std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
           std::to_string(info.st_size) + ":" + std::to_string(modified.tv_sec) + ":" +
           std::to_string(modified.tv_nsec);
#endif
}
}  // namespace supple::engine::detail::files
