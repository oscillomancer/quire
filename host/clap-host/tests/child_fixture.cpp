// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif
int main(int argc, char** argv) {
    if (argc < 2) return 3;
    const std::string mode = argv[1];
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
