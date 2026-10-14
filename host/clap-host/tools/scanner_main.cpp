// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <cstdio>
#include "scan.h"
#include "supple/json/json.h"
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace {
int scan(const std::filesystem::path& module) {
    // Preserve a private binary protocol stream before loading untrusted code.
#if defined(_WIN32)
    const int saved = _dup(_fileno(stdout));
    if (saved < 0) return 1;
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(saved)), HANDLE_FLAG_INHERIT,
                              0)) {
        _close(saved);
        return 1;
    }
    _setmode(saved, _O_BINARY);
    FILE* output = _fdopen(saved, "wb");
    FILE* ignored = nullptr;
    const bool redirected = freopen_s(&ignored, "NUL", "w", stdout) == 0;
    if (redirected)
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
#else
    const int saved = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (saved < 0) return 1;
    FILE* output = ::fdopen(saved, "wb");
    const bool redirected = std::freopen("/dev/null", "w", stdout) != nullptr;
#endif
    if (!output || !redirected) {
        if (output) std::fclose(output);
        return 1;
    }
    std::error_code ec;
    const auto path = std::filesystem::absolute(module, ec).lexically_normal();
    auto result =
        ec ? supple::Result<std::vector<supple::host::PluginInfo>>(supple::Error(
                 "host.scan.load", "cannot resolve plugin path", "/path", "check the module path"))
           : supple::host::scanModuleInProcess(path);
    const auto text = supple::json::writeCanonical(supple::host::scanReply(result));
    const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
    const bool closed = std::fclose(output) == 0;
    return written && closed && result ? 0 : 1;
}
}  // namespace
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 3 || std::wstring_view(argv[1]) != L"--scan") return 2;
    return scan(std::filesystem::path(argv[2]));
}
#else
int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--scan") return 2;
    return scan(std::filesystem::path(argv[2]));
}
#endif
