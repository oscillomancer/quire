// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "help_support.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include "supple/base/paths.h"
#include "supple/host/child_process.h"
#include "supple/json/parse.h"
#include "supple/product.h"
#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace supple::gui::help {
namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
Error failure(std::string message, const fs::path& path) {
    return Error("help.io", std::move(message), utf8(path),
                 "choose another folder or check its permissions");
}
fs::path fromUtf8(std::string_view text) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string env(const char* name) {
#if defined(_WIN32)
    const std::wstring key(name, name + std::char_traits<char>::length(name));
    const DWORD size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(key.c_str(), value.data(), size);
    if (!length || length >= size) return {};
    value.resize(length);
    return utf8(fs::path(value));
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}
Status removeTarget(const fs::path& path);
struct Temp {
    fs::path path;
    ~Temp() {
        if (!path.empty()) (void)removeTarget(path);
    }
};
Result<fs::path> temporary(const fs::path& parent) {
    static std::atomic<unsigned> counter{0};
    for (int i = 0; i < 100; ++i) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = parent / (".supple-help-" + std::to_string(stamp) + "-" +
                                    std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        if (fs::create_directory(path, ec)) return path;
        if (ec) return failure(ec.message(), path);
    }
    return failure("cannot create a temporary folder", parent);
}
bool executableFile(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
#if defined(_WIN32)
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}
bool isLink(const fs::path& path) {
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(path, ec));
#endif
}
Status removeTarget(const fs::path& path) {
    std::error_code ec;
#if defined(_WIN32)
    if (isLink(path)) {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        const bool ok = attrs & FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(path.c_str())
                                                         : DeleteFileW(path.c_str());
        if (!ok)
            return failure("cannot remove link: Windows error " + std::to_string(GetLastError()),
                           path);
        return {};
    }
#endif
    if (!isLink(path) && fs::is_directory(fs::symlink_status(path, ec))) {
        fs::directory_iterator entries(path, ec);
        if (ec) return failure(ec.message(), path);
        for (auto end = fs::directory_iterator(); entries != end; entries.increment(ec)) {
            if (ec) return failure(ec.message(), path);
            if (auto removed = removeTarget(entries->path()); !removed) return removed;
        }
        if (ec) return failure(ec.message(), path);
    }
    ec.clear();
    fs::remove(path, ec);
    if (ec) return failure(ec.message(), path);
    return {};
}
Status directoryLink(const fs::path& source, const fs::path& target) {
#if defined(_WIN32)
    // Mount-point reparse data: a directory junction, requiring no symlink privilege.
    std::wstring print = source.wstring();
    if (print.starts_with(L"\\\\?\\")) print.erase(0, 4);
    const std::wstring substitute = L"\\??\\" + print;
    if (print.starts_with(L"\\\\") || print.starts_with(L"UNC\\"))
        return failure("directory junctions need a local skill folder", source);
    struct Header {
        DWORD tag;
        WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
    };
    const auto names = substitute + L'\0' + print + L'\0';
    if (sizeof(Header) + names.size() * sizeof(wchar_t) > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
        return failure("skill path is too long for a junction", source);
    std::vector<std::byte> bytes(sizeof(Header) + names.size() * sizeof(wchar_t));
    Header header{};
    auto* h = &header;
    h->tag = IO_REPARSE_TAG_MOUNT_POINT;
    h->length = static_cast<WORD>(bytes.size() - 8);
    h->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    h->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    h->printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::memcpy(bytes.data(), &header, sizeof(Header));
    std::memcpy(bytes.data() + sizeof(Header), names.data(), names.size() * sizeof(wchar_t));
    if (!CreateDirectoryW(target.c_str(), nullptr))
        return failure("cannot create junction folder", target);
    const HANDLE handle =
        CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(target.c_str());
        return failure("cannot open junction folder", target);
    }
    DWORD count = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, bytes.data(),
                                    static_cast<DWORD>(bytes.size()), nullptr, 0, &count, nullptr);
    CloseHandle(handle);
    if (!ok) {
        RemoveDirectoryW(target.c_str());
        return failure("cannot create junction: Windows error " + std::to_string(GetLastError()),
                       target);
    }
#else
    std::error_code ec;
    fs::create_directory_symlink(source, target, ec);
    if (ec) return failure(ec.message(), target);
#endif
    return {};
}
}  // namespace

bool isTranslocated(std::string_view executable) {
    return executable.find("/AppTranslocation/") != std::string_view::npos;
}
Platform nativePlatform() {
#if defined(_WIN32)
    return Platform::Windows;
#else
    return Platform::Posix;
#endif
}
fs::path ownExecutable() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring text(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
    if (!size || size == text.size()) return {};
    text.resize(size);
    const fs::path path(text);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string text(size, '\0');
    if (_NSGetExecutablePath(text.data(), &size) != 0) return {};
    const fs::path path(text.c_str());
#else
    const auto path = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
#endif
    return fs::canonical(path, ec);
}
std::string harnessCommand(Harness h) {
    switch (h) {
        case Harness::Claude:
            return "claude";
        case Harness::Codex:
            return "codex";
        case Harness::Hermes:
            return "hermes";
    }
    return {};
}
Executables executables(const fs::path& gui, Platform platform) {
    const auto dir = gui.parent_path();
    const auto root = dir.parent_path().parent_path();
    const std::string suffix = platform == Platform::Windows ? ".exe" : "";
    const auto locate = [&](const std::string& helper, const fs::path& buildDir) {
        const auto file = std::string(product::kSlug) + helper + suffix;
        const auto installed = dir / file;
        std::error_code ec;
        return fs::absolute(fs::exists(installed, ec) ? installed
                            : dir.filename() == "gui" && dir.parent_path().filename() == "daw"
                                ? root / buildDir / file
                                : installed,
                            ec)
            .lexically_normal();
    };
    return {locate("-mcp", "mcp"), locate("-engine", fs::path("host") / "engine")};
}
std::vector<std::string> registrationArgs(Harness h, const std::string& mcp) {
    std::vector<std::string> args{"mcp", "add"};
    if (h == Harness::Claude) args.insert(args.end(), {"-s", "user"});
    args.insert(args.end(),
                {std::string(product::kSlug), h == Harness::Hermes ? "--command" : "--", mcp});
    return args;
}
std::string commandText(const std::string& exe, const std::vector<std::string>& args,
                        Platform platform) {
    auto quote = [](const std::string& s, bool windows) {
        const auto safe = [windows](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   std::string_view("_./:-").find(static_cast<char>(c)) != std::string_view::npos ||
                   (windows && c == '\\');
        };
        if (!s.empty() && std::all_of(s.begin(), s.end(), safe)) return s;
        std::string out = "'";
        for (char c : s) out += c == '\'' ? (windows ? "''" : "'\\''") : std::string(1, c);
        return out + "'";
    };
    const bool win = platform == Platform::Windows;
    const auto command = quote(exe, win);
    // PowerShell requires the call operator for a quoted executable path.
    std::string out = (win && command != exe ? "& " : "") + command;
    for (const auto& arg : args) out += " " + quote(arg, win);
    return out;
}
Result<std::string> windowsScript(const std::string& exe, const std::vector<std::string>& args) {
    auto quote = [](const std::string& s) -> Result<std::string> {
        if (s.find_first_of("\"\r\n\0", 0, 4) != std::string::npos)
            return Error("help.command", "invalid Windows command argument", "/command",
                         "use a path without quotes or newlines");
        std::string out = "\"";
        for (const char c : s) out += c == '%' ? "%%" : std::string(1, c);
        return out + "\"";
    };
    auto q = quote(exe);
    if (!q) return q.error();
    std::string script =
        "@echo off\r\n@chcp 65001 >nul\r\n@setlocal DisableDelayedExpansion\r\n" + *q;
    for (const auto& arg : args) {
        q = quote(arg);
        if (!q) return q.error();
        script += " " + *q;
    }
    return script + " 2>&1\r\n";
}

Result<ProcessResult> runCaptured(const fs::path& exe, const std::vector<std::string>& args,
                                  const std::function<bool()>& cancelled, const std::string& input,
                                  const std::string& searchPath, bool captureStderr) {
    std::vector<std::string> launch;
    fs::path runner;
    auto temp = temporary(fs::temp_directory_path());
    if (!temp) return temp.error();
    Temp cleanup{*temp};
    const fs::path inputFile = cleanup.path / "stdin.txt";
    {
        std::ofstream out(inputFile, std::ios::binary);
        out << input;
        if (!out) return failure("cannot write command input", inputFile);
    }
#if defined(_WIN32)
    (void)searchPath;
    auto script = windowsScript(utf8(exe), args);
    if (!script) return script.error();
    if (!captureStderr) script->replace(script->rfind("2>&1"), 4, "2>nul");
    // Redirect a finite input file: Hermes asks whether to enable discovered tools.
    // A single blank line accepts that default, but never its default-No overwrite.
    // The child needn't inherit a working directory; use the absolute input path.
    std::string quotedInput;
    for (char c : utf8(inputFile)) quotedInput += c == '%' ? "%%" : std::string(1, c);
    script->insert(script->size() - 2, " <\"" + quotedInput + "\"");
    const fs::path file = cleanup.path / "connect.cmd";
    {
        std::ofstream out(file, std::ios::binary);
        out << *script;
        if (!out) return failure("cannot write command", file);
    }
    runner = fromUtf8(env("SystemRoot")) / "System32" / "cmd.exe";
    launch = {"/d", "/v:off", "/s", "/c", utf8(file)};
#else
    // Only a fixed script enters the shell; executable and arguments are positional.
    runner = "/bin/sh";
    launch = {"-c",
              std::string("input=$1; search=$2; shift 2; if [ -n \"$search\" ]; then PATH=$search; "
                          "export PATH; fi; exec \"$@\" < \"$input\" ") +
                  (captureStderr ? "2>&1" : "2>/dev/null"),
              "help-connect",
              utf8(inputFile),
              searchPath,
              utf8(exe)};
    launch.insert(launch.end(), args.begin(), args.end());
#endif
    auto child = host::ChildProcess::spawn(runner, launch);
    if (!child) return child.error();
    std::atomic<bool> finished{false};
    // Cancellation only kills the child. Its owner remains on this thread and reaps it.
    std::thread watcher([&] {
        while (!finished.load()) {
            if (cancelled && cancelled()) {
                (*child)->killTree();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    auto output =
        (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(30), 65536);
    finished.store(true);
    watcher.join();
    if (!output) return output.error();
    std::string excerpt = std::move(output->text);
    if (captureStderr && excerpt.size() > 6000)
        excerpt = excerpt.substr(0, 2500) + "\n… output shortened …\n" +
                  excerpt.substr(excerpt.size() - 3500);
    return ProcessResult{output->exitCode, std::move(excerpt)};
}
fs::path findHarness(Harness h, const std::function<bool()>& cancelled) {
    const auto name = harnessCommand(h);
    std::vector<fs::path> dirs;
    const auto home = paths::home();
    const auto addPath = [&](const std::string& path, char separator) {
        std::size_t begin = 0;
        while (begin <= path.size()) {
            const auto end = path.find(separator, begin);
            const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
            if (!part.empty()) dirs.push_back(fromUtf8(part));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    };
#if defined(_WIN32)
    addPath(env("PATH"), ';');
    dirs.push_back(fromUtf8(env("APPDATA")) / "npm");
    dirs.push_back(fromUtf8(env("LOCALAPPDATA")) / "Programs" / "nodejs");
#else
    addPath(env("PATH"), ':');
#endif
    if (!home.empty()) {
        for (const auto* relative :
             {".local/bin", ".npm-global/bin", ".volta/bin", ".cargo/bin", "bin"})
            dirs.push_back(home / relative);
    }
    for (const auto* dir : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"})
        dirs.emplace_back(dir);
    for (const auto& dir : dirs) {
        if (!dir.is_absolute()) continue;
#if defined(_WIN32)
        for (const auto* suffix : {".exe", ".cmd", ".bat"})
            if (executableFile(dir / (name + suffix))) return dir / (name + suffix);
#else
        if (executableFile(dir / name)) return dir / name;
#endif
    }
#if !defined(_WIN32)
    const auto shell = fromUtf8(env("SHELL"));
    if (shell.is_absolute() && executableFile(shell)) {
        auto output = runCaptured(shell, {"-lc", "command -v " + name}, cancelled);
        if (output && output->exitCode == 0) {
            std::string text = output->text;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            const fs::path candidate = fromUtf8(text.substr(text.find_last_of('\n') + 1));
            if (candidate.is_absolute() && executableFile(candidate)) return candidate;
        }
    }
#endif
    return {};
}
Result<ProcessResult> registerHarness(Harness harness, const fs::path& executable,
                                      const std::string& mcp,
                                      const std::function<bool()>& cancelled) {
    std::string searchPath;
#if defined(__APPLE__)
    // Finder's PATH omits Homebrew/nvm/etc. Preserve the login environment for
    // shebangs such as /usr/bin/env node as well as locating the CLI itself.
    const auto shell = fromUtf8(env("SHELL"));
    if (shell.is_absolute() && executableFile(shell)) {
        auto path = runCaptured(shell, {"-lc", "/usr/bin/printenv PATH"}, cancelled);
        if (path && path->exitCode == 0) {
            searchPath = path->text;
            while (!searchPath.empty() && (searchPath.back() == '\n' || searchPath.back() == '\r'))
                searchPath.pop_back();
            searchPath = searchPath.substr(searchPath.find_last_of('\n') + 1);
        }
    }
#endif
    if (harness == Harness::Codex) {
        // The list is structural; stderr warnings must not corrupt its JSON.
        auto current =
            runCaptured(executable, {"mcp", "list", "--json"}, cancelled, "", searchPath, false);
        if (!current) return current.error();
        if (current->exitCode != 0)
            return runCaptured(executable, {"mcp", "list", "--json"}, cancelled, "", searchPath);
        auto servers = json::parse(current->text);
        if (!servers || !servers->isArray())
            return Error("help.codex_list", "Codex returned an invalid server list", "/command",
                         "run codex mcp list --json in a terminal and check its configuration");
        for (const auto& server : servers->asArray()) {
            const auto* name = server.isObject() ? server.find("name") : nullptr;
            if (!name || !name->isString())
                return Error("help.codex_list", "Codex returned an invalid server entry",
                             "/command",
                             "run codex mcp list --json in a terminal and check its configuration");
            if (name->asString() == product::kSlug) {
                current->text =
                    "Server already exists. Review its path in Codex before replacing it.";
                return current;
            }
        }
    }
    if (cancelled && cancelled())
        return Error("help.cancelled", "connection cancelled", "/command", "try connecting again");
    return runCaptured(executable, registrationArgs(harness, mcp), cancelled,
                       harness == Harness::Hermes ? "\n" : "", searchPath);
}
Status removeExportTree(const fs::path& path) { return removeTarget(path); }
Target inspectTarget(const fs::path& path) {
    if (isLink(path)) return Target::Link;
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found) return Target::Missing;
    if (ec) return Target::Refuse;
    if (fs::is_directory(status) && fs::is_regular_file(path / "SKILL.md", ec))
        return Target::Skill;
    return Target::Refuse;
}
Result<fs::path> exportSkill(const fs::path& source, const fs::path& parent, bool link,
                             bool replace,
                             std::function<Status(const fs::path&, const fs::path&)> publish) {
    std::error_code ec;
    const auto src = fs::canonical(source, ec);
    if (ec || !fs::is_regular_file(src / "SKILL.md", ec))
        return failure("bundled skill is unavailable", source);
    const auto base = fs::canonical(parent, ec);
    if (ec || !fs::is_directory(base, ec)) return failure("choose an existing folder", parent);
    const auto target = base / std::string(product::kSlug);
    // Compare existing ancestors by file identity as well as spelling: this
    // covers case-insensitive Windows paths and directory aliases.
    const auto contains = [](const fs::path& directory, fs::path path) {
        std::error_code error;
        while (!path.empty()) {
            if (fs::equivalent(directory, path, error)) return true;
            const auto next = path.parent_path();
            if (next == path) break;
            path = next;
        }
        return false;
    };
    if (contains(src, base)) return failure("choose a folder outside the bundled skill", target);
    if (!isLink(target) && contains(target, src))
        return failure("cannot replace a folder containing the bundled skill", target);
    const auto kind = inspectTarget(target);
    if (kind == Target::Refuse)
        return failure("refusing to replace a folder without SKILL.md or a regular file", target);
    if (kind != Target::Missing && !replace)
        return Error("help.exists", "a skill or link already exists", target.string(),
                     "confirm replacement first");
    auto temp = temporary(base);
    if (!temp) return temp.error();
    Temp cleanup{*temp};
    const auto staged = cleanup.path / "skill";
    if (link) {
        if (auto made = directoryLink(src, staged); !made) return made.error();
    } else {
        fs::copy(src, staged, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) return failure(ec.message(), staged);
    }
    // Recheck after preparation. A newly created target is never overwritten without consent.
    const auto current = inspectTarget(target);
    if (current == Target::Refuse || (current != Target::Missing && !replace))
        return failure("destination changed; choose it again", target);
    const auto backup = cleanup.path / "previous";
    if (current != Target::Missing) {
        fs::rename(target, backup, ec);
        if (ec) return failure(ec.message(), target);
    }
    auto published = [&]() -> Status {
        if (publish) return publish(staged, target);
        fs::rename(staged, target, ec);
        if (ec) return failure(ec.message(), target);
        return {};
    }();
    if (!published) {
        const auto message = published.error().message;
        if (current != Target::Missing) {
            fs::rename(backup, target, ec);
            if (ec) {
                cleanup.path.clear();
                return failure(message + "; previous skill retained at " + backup.string(), target);
            }
        }
        return failure(message, target);
    }
    if (current != Target::Missing) {
        if (auto removed = removeTarget(backup); !removed) {
            cleanup.path.clear();
            return removed.error();
        }
    }
    return target;
}
}  // namespace supple::gui::help
