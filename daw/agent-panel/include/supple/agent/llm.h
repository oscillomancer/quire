// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Chat completion with tools over HTTP streaming (docs/specs/gui.md §5):
// the Anthropic Messages API or any OpenAI-compatible endpoint (including a
// local llama-server). Requests run on the caller's (worker) thread.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "supple/base/stop.h"

#include "supple/base/result.h"
#include "supple/json/value.h"

namespace supple::agent {

struct Endpoint {
    enum class Kind { Anthropic, OpenAI } kind = Kind::Anthropic;
    std::string name;     // shown in the panel
    std::string baseUrl;  // "https://api.anthropic.com", "http://127.0.0.1:8080/v1"
    std::string model;
    std::string apiKey;  // empty for local servers
    int maxTokens = 4096;
};

struct ToolCall {
    std::string id, name;
    json::Value input = json::Value::object();
};

struct ToolResult {
    std::string id;
    std::string content;  // what the model reads (JSON text)
    bool isError = false;
};

// One conversation message. Assistant messages may carry tool calls; a user
// message may carry the results of the previous assistant's calls.
struct Message {
    enum class Role { User, Assistant } role = Role::User;
    std::string text;
    std::vector<ToolCall> toolCalls;
    std::vector<ToolResult> toolResults;
};

struct ToolDef {
    std::string name, description;
    json::Value inputSchema = json::Value::object();
};

struct Turn {
    Message message;         // the assistant's
    std::string stopReason;  // "end_turn", "tool_use", "max_tokens", …
    int inputTokens = 0, outputTokens = 0;
};

struct Request {
    std::string system;
    std::vector<Message> messages;
    std::vector<ToolDef> tools;
};

// Streams one assistant turn; `onText` receives text as it arrives. The
// stream is checked as it arrives: every event must have its provider's
// types ("agent.stream"), the turn must end with the provider's completion
// marker (Anthropic message_stop, OpenAI "data: [DONE]") and every tool call
// must be complete with object arguments ("agent.stream_incomplete",
// "agent.tool_arguments"), and the response is bounded
// ("agent.stream_too_large"). Any of these fails the turn: a truncated tool
// call never runs.
Result<Turn> complete(const Endpoint& endpoint, const Request& request,
                      const std::function<void(std::string_view)>& onText,
                      supple::StopToken stop = {});

// The bytes one response may stream, and one event may span.
inline constexpr std::size_t kMaxResponseBytes = std::size_t(64) << 20;
inline constexpr std::size_t kMaxEventBytes = std::size_t(8) << 20;

// The JSON bodies (exposed for tests).
json::Value anthropicBody(const Endpoint& endpoint, const Request& request);
json::Value openaiBody(const Endpoint& endpoint, const Request& request);

// Read-only member access: `v[key]` for const values (null when absent).
inline const json::Value& at(const json::Value& v, std::string_view key) {
    static const json::Value null;
    const json::Value* found = v.isObject() ? v.find(key) : nullptr;
    return found ? *found : null;
}

// Server-sent events: feed bytes, get (event, data) pairs. An event (or a
// line) longer than kMaxEventBytes is "agent.stream_too_large".
class SseParser {
  public:
    Status feed(std::string_view bytes,
                const std::function<void(std::string_view event, std::string_view data)>& out);

  private:
    std::string buffer_, event_, data_;
};

}  // namespace supple::agent
