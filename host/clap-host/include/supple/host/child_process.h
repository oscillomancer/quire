// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include "supple/base/result.h"
namespace supple::host {
// Non-RT owning child + process tree. stdin/stderr go to the null device;
// stdout is captured. Only those stdio handles survive exec. Destruction
// kills descendants and reaps the leader even after a collect error.
class ChildProcess {
  public:
    struct Output {
        std::string text;
        std::uint32_t exitCode = 0;
        bool crashed = false;
    };
    struct LaunchOptions {
        // One explicitly inherited bridge control resource. POSIX remaps to fd 3;
        // Windows retains its native value. -1 supplies only the default stdio.
        std::intptr_t control = -1;
        bool captureOutput = true;
    };
    static Result<std::unique_ptr<ChildProcess>> spawn(const std::filesystem::path& executable,
                                                       std::span<const std::string> arguments);
    static Result<std::unique_ptr<ChildProcess>> spawn(const std::filesystem::path& executable,
                                                       std::span<const std::string> arguments,
                                                       LaunchOptions options);
    int pid() const;
    // Borrowed process HANDLE on Windows; 0 on POSIX. Outlives all bridge users.
    std::intptr_t nativeProcess() const;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    Result<Output> collect(std::chrono::steady_clock::time_point deadline, std::size_t maxBytes);
    void killTree();

  private:
    ChildProcess();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace supple::host
