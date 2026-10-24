// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "supple/base/result.h"

namespace supple::gui::help {
enum class Platform { Posix, Windows };
enum class Harness { Claude, Codex, Hermes };
Platform nativePlatform();
bool isTranslocated(std::string_view executable);
std::filesystem::path ownExecutable();
std::string harnessCommand(Harness harness);
struct Executables {
    std::filesystem::path mcp, engine;
};
// Adjacent installed helpers take precedence over the build-tree layout.
Executables executables(const std::filesystem::path& gui, Platform platform);
std::vector<std::string> registrationArgs(Harness harness, const std::string& mcp);
// POSIX shell or Windows PowerShell, suitable for pasting into a terminal.
std::string commandText(const std::string& executable, const std::vector<std::string>& args,
                        Platform platform);
// A batch file, rather than CRT quoting of a cmd /c command string. Percent
// expansion is escaped, delayed expansion is disabled, all arguments are quoted.
Result<std::string> windowsScript(const std::string& executable,
                                  const std::vector<std::string>& args);
std::filesystem::path findHarness(Harness harness, const std::function<bool()>& cancelled = {});
struct ProcessResult {
    unsigned exitCode = 0;
    std::string text;
};
// Non-UI thread only. Bounded output and lifetime; destroys the whole child tree.
// captureStderr=false keeps stdout intact for structural JSON replies.
Result<ProcessResult> runCaptured(const std::filesystem::path& executable,
                                  const std::vector<std::string>& args,
                                  const std::function<bool()>& cancelled = {},
                                  const std::string& input = {}, const std::string& searchPath = {},
                                  bool captureStderr = true);

Result<ProcessResult> registerHarness(Harness harness, const std::filesystem::path& executable,
                                      const std::string& mcp,
                                      const std::function<bool()>& cancelled = {});

// Removes an exported tree without following symlinks or Windows reparse points.
Status removeExportTree(const std::filesystem::path& path);

enum class Target { Missing, Skill, Link, Refuse };
Target inspectTarget(const std::filesystem::path& target);
// Copies or links to <parent>/<product slug>. Existing targets require explicit
// replacement, and ordinary files/directories are never replaced. Link removal
// never traverses the linked tree. Preparation precedes replacement. The optional
// publication operation supports deterministic rollback/failure checks.
Result<std::filesystem::path> exportSkill(
    const std::filesystem::path& source, const std::filesystem::path& parent, bool link,
    bool replace,
    std::function<Status(const std::filesystem::path&, const std::filesystem::path&)> publish = {});
}  // namespace supple::gui::help
