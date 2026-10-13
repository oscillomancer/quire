// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// File references: a property marked .file() (core/Property.h) holds a path,
// and a session that moves -- to another disk, another machine, another
// user -- should still find what it names.
//
// When a saved path no longer exists, relocate() looks for the file:
//
//   1. the same path under this user's home folder, for one saved under
//      another's (/Users/ana/Samples/kick.wav on a machine where you are
//      not ana);
//   2. each folder the user has picked a file from lately (searchFolders(),
//      kept per product in its settings folder) and each folder the caller
//      adds, with as much of the path's tail as matches: Samples/kick.wav,
//      then kick.wav.
//
// A file still not found keeps its path -- saving the session keeps the
// reference -- and is reported Missing until the user points at it, which
// remembers that folder and so finds the rest of a moved library too.
//
// Paths are UTF-8 strings throughout, as properties hold them. Main thread
// (or a worker): everything here touches the file system.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace supple {

// What became of a file property's file, for the editor.
struct FileStatus {
    enum class State : uint8_t {
        Empty,    // no file chosen
        Loading,  // being read
        Ready,    // loaded; the processor has it
        Missing,  // the path names nothing, and relocate() found it nowhere
        Failed,   // it exists but could not be loaded: `error` says why
    };
    State state = State::Empty;
    std::string path;     // as the property holds it
    std::string error;    // Failed: why
    std::string summary;  // Ready: a line about it ("2 ch, 48 kHz, 1.50 s")
};

// Everything a FileProcessor's load() hears about the session.
struct FileRequest {
    uint32_t id = 0;    // the property
    uint32_t kind = 0;  // its PropertyDef::kind
    std::string path;
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int channels = 2;
};

namespace files {

// A path from UTF-8 text and back, the same on every system. Neither throws:
// what cannot be converted (bytes that are not UTF-8, on Windows) is empty.
std::filesystem::path fromUtf8(std::string_view text);
std::string toUtf8(const std::filesystem::path& path);

bool exists(std::string_view path);
// An exclusive adjacent temporary file, closed before replacing the target.
// Concurrent writers replace whole files; the last successful replacement wins.
bool writeAtomic(const std::filesystem::path& path, const uint8_t* bytes, size_t size);
// Reject special files and oversized input before opening or allocating.
bool readBounded(const std::filesystem::path& path, std::vector<uint8_t>& bytes, size_t maxBytes);
// The last component, whichever separator the path was saved with.
std::string fileName(std::string_view path);
// The folder holding it.
std::string folderOf(std::string_view path);
// `extensions`: space-separated, without dots ("wav flac"); empty accepts
// anything. Case-insensitive.
bool hasExtension(std::string_view path, std::string_view extensions);

// Where a missing file went (see above), or "" when nowhere. A path that
// exists is returned as it is.
std::string relocate(std::string_view path, const std::vector<std::string>& extraFolders = {});

// The product's own per-user settings folder:
//   macOS    ~/Library/Application Support/<vendor>/<product>
//   Windows  %APPDATA%\<vendor>\<product>
//   Linux    $XDG_CONFIG_HOME (or ~/.config)/<vendor>/<product>
// SUPPLE_SETTINGS_DIR in the environment replaces them all (for tests).
std::filesystem::path settingsFolder();
// Folders recently picked from, newest first, at most 16.
std::vector<std::string> searchFolders();
void rememberFolder(std::string_view folder);

}  // namespace files
}  // namespace supple
