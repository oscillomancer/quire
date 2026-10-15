// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/agent/config.h"
#include <supple/base/paths.h>

#include "private_config.h"

#include <cerrno>
#include <cstdlib>
#include <system_error>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::agent {
std::filesystem::path Config::defaultFile(std::string_view slug) {
    if (const auto root = paths::config(); !root.empty()) return root / slug / "agent.json";
    return {};
}

Result<Config> Config::load(const std::filesystem::path& file) {
    Config c;
    if (file.empty() || !std::filesystem::exists(file)) return c;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    if (at(*doc, "schema").isString() == false ||
        at(*doc, "schema").asString() != "supple.agent/1.0")
        return Error("agent.config", file.string() + " is not a supple.agent/1.0 document",
                     "/schema");
    const json::Value& list = at(*doc, "endpoints");
    if (list.isArray())
        for (std::size_t i = 0; i < list.asArray().size(); ++i) {
            const json::Value& e = list.asArray()[i];
            const std::string path = "/endpoints/" + std::to_string(i);
            Endpoint ep;
            const std::string kind =
                at(e, "kind").isString() ? at(e, "kind").asString() : "anthropic";
            if (kind != "anthropic" && kind != "openai")
                return Error("agent.config", "kind is \"anthropic\" or \"openai\"", path + "/kind");
            ep.kind = kind == "openai" ? Endpoint::Kind::OpenAI : Endpoint::Kind::Anthropic;
            for (auto [key, field] : {std::pair{"name", &ep.name},
                                      {"base_url", &ep.baseUrl},
                                      {"model", &ep.model},
                                      {"api_key", &ep.apiKey}})
                if (at(e, key).isString()) *field = at(e, key).asString();
            if (at(e, "max_tokens").isNumber())
                ep.maxTokens = static_cast<int>(at(e, "max_tokens").asInt());
            c.endpoints.push_back(std::move(ep));
        }
    if (at(*doc, "current").isNumber())
        c.current = static_cast<std::size_t>(at(*doc, "current").asInt());
    return c;
}

Status Config::save(const std::filesystem::path& file) const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.agent/1.0";
    json::Value list = json::Value::array();
    for (const Endpoint& e : endpoints) {
        json::Value o = json::Value::object();
        o["name"] = e.name;
        o["kind"] = e.kind == Endpoint::Kind::OpenAI ? "openai" : "anthropic";
        o["base_url"] = e.baseUrl;
        o["model"] = e.model;
        o["api_key"] = e.apiKey;
        o["max_tokens"] = e.maxTokens;
        list.push_back(std::move(o));
    }
    doc["endpoints"] = std::move(list);
    doc["current"] = static_cast<std::int64_t>(current);
    return writePrivateConfig(file, json::writeCanonical(doc) + "\n");
}

}  // namespace supple::agent
