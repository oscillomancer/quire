// Agent endpoints, stored per user in $XDG_CONFIG_HOME/<slug>/agent.json
// (supple.agent/1.0), readable by the owner only (0600): API keys live here,
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
    // Atomic and owner-only throughout: a 0600 temporary file in the same
    // directory, fsynced, then renamed over `file`; a failure (code
    // "agent.config") leaves the previous file as it was.
    Status save(const std::filesystem::path& file) const;
};

}  // namespace supple::agent
