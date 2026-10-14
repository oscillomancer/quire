// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/host/child_process.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) return 3;
    const std::string mode = argv[1];
    if (mode == "stderr") {
        std::fputs("child stderr", stderr);
        return std::fflush(stderr) == 0 ? 0 : 20;
    }
    // Isolate changes to the parent's stderr from the test runner and its threads.
    if (mode == "stderr-parent" && argc == 5) {
        FILE* file = std::fopen(argv[2], "wb");
        if (!file) return 21;
        struct Close {
            FILE* file;
            ~Close() { std::fclose(file); }
        } close{file};
#if defined(_WIN32)
        const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
        if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0) ||
            !SetStdHandle(STD_ERROR_HANDLE, handle))
            return 22;
#else
        if (::dup2(::fileno(file), STDERR_FILENO) != STDERR_FILENO) return 22;
#endif
        const std::array<std::string, 1> args{"stderr"};
        supple::host::ChildProcess::LaunchOptions options;
        if (std::string(argv[3]) == "1") options.inheritStderr = true;
        if (std::string(argv[4]) == "0") options.captureOutput = false;
        auto child = supple::host::ChildProcess::spawn(argv[0], args, options);
        if (!child) return 23;
#if defined(_WIN32)
        DWORD flags = 0;
        if (!GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT)) return 25;
#endif
        auto output =
            (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(5), 32);
        return output && output->exitCode == 0 && output->text.empty() ? 0 : 24;
    }
#if !defined(_WIN32)
    if (mode == "control-stdio") {
        int fds[2];
        if (::pipe(fds) != 0) return 10;
        for (int fd : fds)
            if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return 11;
        if (::dup2(fds[0], STDOUT_FILENO) != STDOUT_FILENO) return 12;
        ::close(fds[0]);
        ::close(STDIN_FILENO);
        const std::array<std::string, 2> args{"-c", "cat <&3"};
        supple::host::ChildProcess::LaunchOptions options;
        options.control = STDOUT_FILENO;
        auto child = supple::host::ChildProcess::spawn("/bin/sh", args, options);
        if (!child) return 13;
        if (::write(fds[1], "ipc", 3) != 3) return 14;
        ::close(fds[1]);
        ::close(STDOUT_FILENO);
        auto output =
            (*child)->collect(std::chrono::steady_clock::now() + std::chrono::seconds(5), 32);
        return output && output->exitCode == 0 && output->text == "ipc" ? 0 : 15;
    }
#endif
#if defined(_WIN32)
    if (mode == "tree" && argc == 3) {
        std::wstring exe(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (!length || length == exe.size()) return 4;
        exe.resize(length);
        std::wstring command = L"\"" + exe + L"\" hang";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &child))
            return 5;
        std::ofstream(argv[2]) << child.dwProcessId;
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return 0;
    }
#endif
    if (mode == "hang")
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(60));
    if (mode == "flood")
        for (;;) {
            std::fputs("12345678901234567890", stdout);
            std::fflush(stdout);
        }
    for (int i = 2; i < argc; ++i) {
        std::fputs(argv[i], stdout);
        std::fputc('\n', stdout);
    }
    return mode == "failure" ? 7 : 0;
}
