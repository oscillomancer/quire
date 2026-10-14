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
    static Result<std::unique_ptr<ChildProcess>> spawn(const std::filesystem::path& executable,
                                                       std::span<const std::string> arguments);
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
