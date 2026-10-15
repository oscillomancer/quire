// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "supple/base/files.h"
#include "supple/script_export.hpp"
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include "supple/host/child_process.h"
#endif

namespace supple::dsp_commands::binary_export {
namespace fs = std::filesystem;

inline fs::path path(std::string_view text) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
inline std::string text(const fs::path& value) {
    const auto bytes = value.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Respect CFBundleExecutable rather than guessing from the renamed bundle.
inline Result<fs::path> moduleFile(const fs::path& module) {
#if defined(__APPLE__)
    std::error_code ec;
    if (fs::is_directory(module, ec)) {
        const auto name = text(module);
        auto url = CFURLCreateFromFileSystemRepresentation(
            nullptr, reinterpret_cast<const UInt8*>(name.data()), static_cast<CFIndex>(name.size()),
            true);
        auto bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
        auto executable = bundle ? CFBundleCopyExecutableURL(bundle) : nullptr;
        UInt8 buffer[32768];
        const bool ok =
            executable && CFURLGetFileSystemRepresentation(executable, true, buffer,
                                                           static_cast<CFIndex>(sizeof buffer));
        fs::path file;
        if (ok) file = path(reinterpret_cast<const char*>(buffer));
        if (executable) CFRelease(executable);
        if (bundle) CFRelease(bundle);
        if (url) CFRelease(url);
        if (!ok)
            return Error("dsp.player", "the player bundle has no executable", "/script",
                         "restore Contents/Info.plist and its CFBundleExecutable");
        return file;
    }
#endif
    return module;
}

#if defined(__APPLE__)
inline Result<std::int64_t> writeBundle(const fs::path& module, const fs::path& file,
                                        std::string_view payload) {
    std::error_code ec;
    if (!fs::is_directory(module, ec))
        return Error("dsp.player", "macOS player exports require a CLAP bundle", "/script",
                     "restore the script player's complete .clap bundle");
    fs::create_directories(file.parent_path(), ec);
    auto failure = [&](std::string action) {
        return Error("dsp.path", std::move(action) + ": " + ec.message(), "/folder",
                     "choose a writable export folder");
    };
    if (ec) return failure("cannot create export folder");
    struct Temporary {
        fs::path folder;
        bool owned = false;
        bool preserve = false;
        ~Temporary() {
            std::error_code ignored;
            if (owned && !preserve) fs::remove_all(folder, ignored);
        }
    } temporary;
    static std::atomic<unsigned> serial{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        temporary.folder = file.parent_path() / (".script-export-" + std::to_string(stamp) + "-" +
                                                 std::to_string(serial.fetch_add(1)));
        if (fs::create_directory(temporary.folder, ec)) {
            temporary.owned = true;
            break;
        }
        if (ec) return failure("cannot stage export");
        temporary.folder.clear();
    }
    if (temporary.folder.empty())
        return Error("dsp.path", "cannot reserve an export staging folder", "/folder",
                     "retry exporting to a writable folder");
    const fs::path staged = temporary.folder / file.filename();
    const auto source = fs::canonical(module, ec);
    if (ec) return failure("cannot resolve player bundle");
    fs::copy(source, staged, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) return failure("cannot copy player bundle");
    const auto stagedRoot = fs::canonical(staged, ec);
    if (ec) return failure("cannot resolve staged bundle");
    for (fs::recursive_directory_iterator it(staged, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_symlink(ec)) continue;
        const auto target = fs::canonical(it->path(), ec);
        if (ec) break;
        const auto relative = target.lexically_relative(stagedRoot);
        if (relative.empty() || *relative.begin() == "..")
            return Error("dsp.player", "the player bundle has a link outside its contents",
                         "/script", "restore a self-contained player bundle before exporting");
    }
    if (ec) return failure("cannot inspect bundle links");
    SUPPLE_TRY(writeFileAtomic(staged / "Contents/Resources" / scriptc::kExportResource, payload));

    // Sign the entire copied bundle after adding its resource. Never append to
    // the Mach-O: arm64 checks its signature before the player can read anything.
    const std::string arguments[] = {"--force", "--sign", "-", "--timestamp=none", text(staged)};
    auto signer = host::ChildProcess::spawn("/usr/bin/codesign", arguments);
    if (!signer)
        return Error("dsp.sign", "cannot start /usr/bin/codesign", "/folder",
                     "restore the system codesign tool, or use format source");
    auto signedBundle =
        (*signer)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(30), 65536);
    if (!signedBundle || signedBundle->crashed || signedBundle->exitCode != 0)
        return Error("dsp.sign", "ad-hoc signing the exported bundle failed", "/folder",
                     "check /usr/bin/codesign --force --sign - on a copy of the player; "
                     "restore the player bundle or use format source");

    std::int64_t bytes = 0;
    for (fs::recursive_directory_iterator it(staged, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) bytes += static_cast<std::int64_t>(it->file_size(ec));
    }
    if (ec) return failure("cannot measure exported bundle");
    const fs::path previous = temporary.folder / "previous";
    const bool replacing = fs::exists(file, ec);
    if (ec) return failure("cannot inspect export destination");
    if (replacing) {
        fs::rename(file, previous, ec);
        if (ec) return failure("cannot replace previous export");
    }
    fs::rename(staged, file, ec);
    if (ec) {
        const auto publishError = failure("cannot publish exported bundle");
        if (replacing) {
            std::error_code restore;
            fs::rename(previous, file, restore);
            if (restore) {
                temporary.preserve = true;
                return Error("dsp.path", "cannot publish export or restore its previous bundle",
                             "/folder", "recover the previous export from " + text(previous));
            }
        }
        return publishError;
    }
    return bytes;
}
#endif
}  // namespace supple::dsp_commands::binary_export
