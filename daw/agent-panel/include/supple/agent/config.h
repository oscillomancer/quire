// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Agent endpoints, stored per user in $XDG_CONFIG_HOME/<slug>/agent.json
// (supple.agent/1.0), owner-only (POSIX 0600 / protected Windows DACL): API keys live here,
// never in a project.
#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "supple/agent/llm.h"
#include "supple/base/result.h"

namespace supple::agent {

struct Config {
    std::vector<Endpoint> endpoints;
    std::size_t current = 0;
    const Endpoint* active() const {
        return current < endpoints.size() ? &endpoints[current] : nullptr;
    }

    static std::filesystem::path defaultFile(std::string_view slug);
    static Result<Config> load(const std::filesystem::path& file);  // empty when absent
    // Atomic and owner-only throughout: a private temporary file in the same
    // directory, fsynced, then renamed over `file`; a failure (code
    // "agent.config") leaves the previous file as it was.
    Status save(const std::filesystem::path& file) const;
};

}  // namespace supple::agent
