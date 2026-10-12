// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "core/Files.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "ProductInfo.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace supple::files {

namespace {

constexpr size_t kRemembered = 16;
constexpr int kTailDepth = 4;  // how much of a path's tail is tried under each folder

// A saved path's components, split on either separator: it may have been
// saved on another system.
std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(part);
    return parts;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

fs::path home() {
#ifdef _WIN32
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE"); profile && *profile) return fs::path(profile);
    return {};
#else
    const char* h = std::getenv("HOME");
    return h && *h ? fromUtf8(h) : fs::path();
#endif
}

// Under some user's home: the components after it, or none.
bool underAHome(const std::vector<std::string>& parts, size_t& rest) {
    for (size_t i = 0; i + 2 < parts.size(); ++i) {
        const std::string p = lower(parts[i]);
        if (p == "home" || p == "users") {
            rest = i + 2;  // skip "home" and the user's name
            return true;
        }
    }
    return false;
}

fs::path join(fs::path base, const std::vector<std::string>& parts, size_t from) {
    for (size_t i = from; i < parts.size(); ++i) base /= fromUtf8(parts[i]);
    return base;
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

fs::path remembered() { return settingsFolder() / "file-folders.txt"; }

}  // namespace

// Windows converts to and from UTF-16 here, and throws on text that is not
// UTF-8 or a name that is not UTF-16; a saved path or a host's is either.
fs::path fromUtf8(std::string_view text) {
    try {
        return fs::path(std::u8string(text.begin(), text.end()));
    } catch (...) {
        return {};
    }
}

std::string toUtf8(const fs::path& path) {
    try {
        const std::u8string u = path.u8string();
        return std::string(u.begin(), u.end());
    } catch (...) {
        return {};
    }
}

bool exists(std::string_view path) { return !path.empty() && isFile(fromUtf8(path)); }

bool writeAtomic(const fs::path& path, const uint8_t* bytes, size_t size) {
    static std::atomic<uint64_t> sequence{0};
    fs::path temp;
    std::FILE* out = nullptr;
    for (int attempt = 0; attempt < 64 && !out; ++attempt) {
        temp = path;
        temp += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
        out = _wfopen(temp.c_str(), L"wbx");
#else
        out = std::fopen(temp.c_str(), "wbx");
#endif
        if (!out && errno != EEXIST) return false;
    }
    if (!out) return false;
    const bool written = std::fwrite(bytes, 1, size, out) == size;
    const bool closed = std::fclose(out) == 0;
    std::error_code ec;
    bool replaced = false;
    if (written && closed) {
#ifdef _WIN32
        // Windows refuses a replace while another is under way on the same
        // file, or a reader has it open without FILE_SHARE_DELETE: wait for
        // it, briefly, as a POSIX rename never has to.
        for (int attempt = 0; attempt < 100 && !replaced; ++attempt) {
            replaced = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            const DWORD error = replaced ? 0 : GetLastError();
            if (!replaced && error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
            if (!replaced) Sleep(1);
        }
#else
        fs::rename(temp, path, ec);
        replaced = !ec;
#endif
    }
    if (!replaced) fs::remove(temp, ec);
    return replaced;
}

bool readBounded(const fs::path& path, std::vector<uint8_t>& bytes, size_t maxBytes) {
    bytes.clear();
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    const auto size = fs::file_size(path, ec);
    if (ec || size > maxBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.resize(static_cast<size_t>(size));
    if (size) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return in && in.peek() == std::char_traits<char>::eof();
}

std::string fileName(std::string_view path) {
    const std::vector<std::string> parts = components(path);
    return parts.empty() ? std::string() : parts.back();
}

std::string folderOf(std::string_view path) { return toUtf8(fromUtf8(path).parent_path()); }

bool hasExtension(std::string_view path, std::string_view extensions) {
    if (extensions.empty()) return true;
    const std::string name = lower(fileName(path));
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = name.substr(dot + 1);
    size_t start = 0;
    while (start < extensions.size()) {
        size_t end = extensions.find(' ', start);
        if (end == std::string_view::npos) end = extensions.size();
        if (end > start && lower(extensions.substr(start, end - start)) == ext) return true;
        start = end + 1;
    }
    return false;
}

std::string relocate(std::string_view path, const std::vector<std::string>& extraFolders) {
    if (path.empty()) return {};
    if (exists(path)) return std::string(path);
    const std::vector<std::string> parts = components(path);
    if (parts.empty()) return {};

    // 1. Another user's home, here.
    size_t rest = 0;
    if (const fs::path h = home(); !h.empty() && underAHome(parts, rest)) {
        const fs::path candidate = join(h, parts, rest);
        if (isFile(candidate)) return toUtf8(candidate);
    }

    // 2. Folders, with the longest tail that matches.
    std::vector<std::string> folders = extraFolders;
    for (const std::string& f : searchFolders()) folders.push_back(f);
    for (const std::string& folder : folders) {
        if (folder.empty()) continue;
        const fs::path base = fromUtf8(folder);
        const size_t deepest = std::min<size_t>(kTailDepth, parts.size());
        for (size_t depth = deepest; depth >= 1; --depth) {
            const fs::path candidate = join(base, parts, parts.size() - depth);
            if (isFile(candidate)) return toUtf8(candidate);
        }
    }
    return {};
}

fs::path settingsFolder() {
    if (const char* forced = std::getenv("SUPPLE_SETTINGS_DIR"); forced && *forced) return fromUtf8(forced);
    const fs::path product = fromUtf8(product::info::kVendor) / fromUtf8(product::info::kName);
#if defined(_WIN32)
    if (const wchar_t* appData = _wgetenv(L"APPDATA"); appData && *appData) return fs::path(appData) / product;
    return {};
#elif defined(__APPLE__)
    const fs::path h = home();
    return h.empty() ? fs::path() : h / "Library" / "Application Support" / product;
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config == '/') return fromUtf8(config) / product;
    const fs::path h = home();
    return h.empty() ? fs::path() : h / ".config" / product;
#endif
}

std::vector<std::string> searchFolders() {
    std::vector<std::string> folders;
    if (settingsFolder().empty()) return folders;
    std::ifstream in(remembered(), std::ios::binary);
    std::string line;
    while (folders.size() < kRemembered && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) folders.push_back(line);
    }
    return folders;
}

void rememberFolder(std::string_view folder) {
    if (folder.empty() || settingsFolder().empty()) return;
    std::vector<std::string> folders = searchFolders();
    std::erase(folders, std::string(folder));
    folders.insert(folders.begin(), std::string(folder));
    if (folders.size() > kRemembered) folders.resize(kRemembered);
    std::error_code ec;
    fs::create_directories(settingsFolder(), ec);
    std::string text;
    for (const std::string& f : folders) text += f + '\n';
    writeAtomic(remembered(), reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

}  // namespace supple::files
