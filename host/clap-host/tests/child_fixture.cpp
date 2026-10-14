// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
int main(int argc, char** argv) {
    if (argc < 2) return 3;
    const std::string mode = argv[1];
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
