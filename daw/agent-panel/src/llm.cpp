// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/agent/llm.h"

#include <curl/curl.h>

#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::agent {
namespace {

void initCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Tool arguments must be a JSON object.
Result<json::Value> parseObject(const std::string& raw) {
    if (raw.empty())
        return Error("agent.tool_arguments", "the tool call's arguments are missing", {},
                     "the stream ended or was cut before the arguments arrived; send again");
    auto v = json::parse(raw);
    if (!v)
        return Error("agent.tool_arguments", "invalid tool argument JSON", {},
                     v.error().toString());
    if (!v->isObject()) return Error("agent.tool_arguments", "tool arguments must be an object");
    return std::move(*v);
}

// A nonnegative integer that fits an int (stream indices).
std::optional<int> intIndex(const json::Value& v) {
    if (!v.isNumber()) return std::nullopt;
    const double d = v.asDouble();
    if (!std::isfinite(d) || d < 0 || d > std::numeric_limits<int>::max() || std::floor(d) != d)
        return std::nullopt;
    return static_cast<int>(d);
}

// A token count; anything else leaves `out` as it is.
void tokens(const json::Value& v, int& out) {
    if (auto n = intIndex(v)) out = *n;
}

Error malformed(const std::string& what) {
    return Error("agent.stream", "malformed stream event: " + what, {},
                 "the endpoint does not stream its API's events; check its base URL and kind");
}

Error incomplete(const std::string& what) {
    return Error("agent.stream_incomplete", "the stream ended early: " + what, {},
                 "the connection was cut or the server stopped; send again");
}

std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// --- streaming POST

using OnBytes = std::function<Status(std::string_view)>;

struct Stream {
    const OnBytes& onBytes;
    std::stop_token stop;
    CURL* curl = nullptr;
    std::string head;  // the first bytes, kept for error messages
    std::size_t total = 0;
    std::optional<Error> failure;  // why the body was refused
    long status = 0;
};

// Returning less than `n` aborts the transfer.
size_t onWrite(char* data, size_t size, size_t count, void* user) {
    auto* s = static_cast<Stream*>(user);
    const size_t n = size * count;
    if (s->stop.stop_requested()) return 0;
    if (s->head.size() < 2048) s->head.append(data, std::min(n, 2048 - s->head.size()));
    long status = 0;
    curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &status);
    s->total += n;
    if (s->total > kMaxResponseBytes) {
        s->failure =
            Error("agent.stream_too_large",
                  "the response is larger than " + std::to_string(kMaxResponseBytes >> 20) + " MiB",
                  {}, "lower max_tokens or check the endpoint");
        return 0;
    }
    if (status < 200 || status >= 300) return n;  // an error body: reported from `head`
    if (auto fed = s->onBytes(std::string_view(data, n)); !fed) {
        s->failure = std::move(fed).error();
        return 0;
    }
    return n;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Stream*>(user)->stop.stop_requested() ? 1 : 0;
}

Status post(const std::string& url, const std::vector<std::string>& headers,
            const std::string& body, const OnBytes& onBytes, std::stop_token stop) {
    initCurl();
    CURL* curl = curl_easy_init();
    if (!curl) return Error("agent.http", "could not start an HTTP client");
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    Stream s{onBytes, stop, curl, {}, 0, std::nullopt, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  // abort after 120 s without data
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &s.status);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    if (stop.stop_requested()) return Error("agent.cancelled", "the request was cancelled");
    if (s.failure) return std::move(*s.failure);
    if (s.status != 0 && (s.status < 200 || s.status >= 300))
        return Error("agent.http", "HTTP " + std::to_string(s.status) + ": " + s.head, url,
                     s.status == 401 || s.status == 403 ? "check the API key" : "");
    if (rc != CURLE_OK)
        return Error("agent.http", std::string("request failed: ") + curl_easy_strerror(rc), url);
    return {};
}

}  // namespace

// --- SSE

Status SseParser::feed(std::string_view bytes,
                       const std::function<void(std::string_view, std::string_view)>& out) {
    const auto tooLarge = [] {
        return Error(
            "agent.stream_too_large",
            "a server-sent event is larger than " + std::to_string(kMaxEventBytes >> 20) + " MiB",
            {}, "check the endpoint");
    };
    buffer_.append(bytes);
    std::size_t start = 0;
    for (;;) {
        const auto nl = buffer_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string_view line(buffer_.data() + start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = nl + 1;
        if (line.empty()) {  // dispatch
            if (!data_.empty()) out(event_, data_);
            event_.clear();
            data_.clear();
        } else if (line.rfind("data:", 0) == 0) {
            std::string_view v = line.substr(5);
            if (!v.empty() && v.front() == ' ') v.remove_prefix(1);
            if (!data_.empty()) data_ += '\n';
            data_ += v;
            if (event_.size() + data_.size() > kMaxEventBytes) return tooLarge();
        } else if (line.rfind("event:", 0) == 0) {
            std::string_view v = line.substr(6);
            if (!v.empty() && v.front() == ' ') v.remove_prefix(1);
            if (v.size() + data_.size() > kMaxEventBytes) return tooLarge();
            event_ = std::string(v);
        }  // comments (":") and other fields are ignored
    }
    buffer_.erase(0, start);
    if (buffer_.size() + event_.size() + data_.size() > kMaxEventBytes) return tooLarge();
    return {};
}

// --- Anthropic

json::Value anthropicBody(const Endpoint& e, const Request& r) {
    json::Value body = json::Value::object();
    body["model"] = e.model;
    body["max_tokens"] = e.maxTokens;
    body["stream"] = true;
    if (!r.system.empty()) body["system"] = r.system;
    json::Value messages = json::Value::array();
    for (const Message& m : r.messages) {
        json::Value msg = json::Value::object();
        msg["role"] = m.role == Message::Role::User ? "user" : "assistant";
        json::Value content = json::Value::array();
        for (const ToolResult& t : m.toolResults) {
            json::Value block = json::Value::object();
            block["type"] = "tool_result";
            block["tool_use_id"] = t.id;
            block["content"] = t.content;
            if (t.isError) block["is_error"] = true;
            content.push_back(std::move(block));
        }
        if (!m.text.empty()) {
            json::Value block = json::Value::object();
            block["type"] = "text";
            block["text"] = m.text;
            content.push_back(std::move(block));
        }
        for (const ToolCall& c : m.toolCalls) {
            json::Value block = json::Value::object();
            block["type"] = "tool_use";
            block["id"] = c.id;
            block["name"] = c.name;
            block["input"] = c.input;
            content.push_back(std::move(block));
        }
        msg["content"] = std::move(content);
        messages.push_back(std::move(msg));
    }
    body["messages"] = std::move(messages);
    if (!r.tools.empty()) {
        json::Value tools = json::Value::array();
        for (const ToolDef& t : r.tools) {
            json::Value tool = json::Value::object();
            tool["name"] = t.name;
            tool["description"] = t.description;
            tool["input_schema"] = t.inputSchema;
            tools.push_back(std::move(tool));
        }
        body["tools"] = std::move(tools);
    }
    return body;
}

namespace {

Result<Turn> anthropic(const Endpoint& e, const Request& r,
                       const std::function<void(std::string_view)>& onText, std::stop_token stop) {
    Turn turn;
    turn.message.role = Message::Role::Assistant;
    struct ToolBlock {
        std::size_t call = 0;  // in turn.message.toolCalls
        json::Value start;     // content_block_start's input (used when no deltas arrive)
        std::string partial;   // input_json_delta pieces
        bool stopped = false;
    };
    std::map<int, ToolBlock> toolBlocks;  // by block index
    std::set<int> started, stopped;       // every block
    bool done = false;                    // message_stop
    std::optional<Error> streamError;
    auto fail = [&](Error error) {
        if (!streamError) streamError = std::move(error);
    };
    SseParser sse;
    auto onEvent = [&](std::string_view, std::string_view data) {
        if (streamError || done) return;
        auto parsed = json::parse(data);
        if (!parsed || !parsed->isObject() || !at(*parsed, "type").isString())
            return fail(malformed("expected an object with a string \"type\""));
        const json::Value& ev = *parsed;
        const std::string& type = at(ev, "type").asString();
        if (type == "content_block_start" || type == "content_block_delta" ||
            type == "content_block_stop") {
            const auto index = intIndex(at(ev, "index"));
            if (!index) return fail(malformed(type + " needs a nonnegative integer index"));
            const std::string block = "content block " + std::to_string(*index);
            if (type == "content_block_start") {
                const json::Value& content = at(ev, "content_block");
                if (!content.isObject() || !at(content, "type").isString())
                    return fail(malformed(block + " needs a typed content_block"));
                if (!started.insert(*index).second)
                    return fail(malformed(block + " started twice"));
                if (at(content, "type").asString() != "tool_use") return;
                const json::Value &id = at(content, "id"), &name = at(content, "name"),
                                  &input = at(content, "input");
                if (!id.isString() || !name.isString() || name.asString().empty())
                    return fail(malformed(block + ": tool_use needs a string id and name"));
                if (!input.isNull() && !input.isObject())
                    return fail(malformed(block + ": tool_use input must be an object"));
                toolBlocks[*index] = {turn.message.toolCalls.size(),
                                      input.isObject() ? input : json::Value::object(),
                                      {},
                                      false};
                turn.message.toolCalls.push_back({id.asString(), name.asString(), {}});
                return;
            }
            if (!started.count(*index) || stopped.count(*index))
                return fail(malformed(type + " for " + block + ", which is not open"));
            if (type == "content_block_stop") {
                stopped.insert(*index);
                auto it = toolBlocks.find(*index);
                if (it == toolBlocks.end()) return;
                ToolBlock& tb = it->second;
                tb.stopped = true;
                if (tb.partial.empty()) {
                    turn.message.toolCalls[tb.call].input = std::move(tb.start);
                    return;
                }
                auto input = parseObject(tb.partial);
                if (!input) return fail(std::move(input).error());
                turn.message.toolCalls[tb.call].input = std::move(*input);
                return;
            }
            const json::Value& delta = at(ev, "delta");
            if (!delta.isObject() || !at(delta, "type").isString())
                return fail(malformed(block + ": the delta needs a string type"));
            const std::string& dt = at(delta, "type").asString();
            if (dt == "text_delta") {
                if (!at(delta, "text").isString())
                    return fail(malformed(block + ": text_delta needs a string text"));
                const std::string& t = at(delta, "text").asString();
                turn.message.text += t;
                if (onText) onText(t);
            } else if (dt == "input_json_delta") {
                auto it = toolBlocks.find(*index);
                if (it == toolBlocks.end())
                    return fail(malformed(block + ": input_json_delta outside a tool_use block"));
                if (!at(delta, "partial_json").isString())
                    return fail(
                        malformed(block + ": input_json_delta needs a string partial_json"));
                it->second.partial += at(delta, "partial_json").asString();
            }  // other deltas (thinking, signatures, citations) carry nothing the panel uses
        } else if (type == "message_start") {
            const json::Value& message = at(ev, "message");
            if (!message.isObject()) return fail(malformed("message_start needs a message object"));
            tokens(at(at(message, "usage"), "input_tokens"), turn.inputTokens);
        } else if (type == "message_delta") {
            const json::Value& delta = at(ev, "delta");
            if (!delta.isObject()) return fail(malformed("message_delta needs a delta object"));
            const json::Value& reason = at(delta, "stop_reason");
            if (!reason.isNull() && !reason.isString())
                return fail(malformed("stop_reason must be a string or null"));
            if (reason.isString()) turn.stopReason = reason.asString();
            tokens(at(at(ev, "usage"), "output_tokens"), turn.outputTokens);
        } else if (type == "message_stop") {
            done = true;
        } else if (type == "error") {
            const json::Value& message = at(at(ev, "error"), "message");
            fail(Error("agent.api",
                       message.isString() ? message.asString() : json::writeCompact(ev)));
        }  // ping and event types added later are ignored
    };
    std::vector<std::string> headers = {"content-type: application/json",
                                        "anthropic-version: 2023-06-01",
                                        "accept: text/event-stream"};
    if (!e.apiKey.empty()) headers.push_back("x-api-key: " + e.apiKey);
    auto status = post(
        trimSlash(e.baseUrl) + "/v1/messages", headers, json::writeCompact(anthropicBody(e, r)),
        [&](std::string_view bytes) -> Status {
            if (auto fed = sse.feed(bytes, onEvent); !fed) return fed;
            if (streamError) return *streamError;  // stop reading
            return {};
        },
        stop);
    if (!status) return std::move(status).error();
    if (streamError) return *streamError;
    if (!done) return incomplete("no message_stop");
    for (const auto& [index, tb] : toolBlocks)
        if (!tb.stopped)
            return incomplete("tool call " + turn.message.toolCalls[tb.call].name +
                              " (content block " + std::to_string(index) + ") was not finished");
    if (turn.stopReason.empty())
        turn.stopReason = turn.message.toolCalls.empty() ? "end_turn" : "tool_use";
    return turn;
}

}  // namespace

// --- OpenAI-compatible

namespace {
// The schema without regex "pattern"s. llama.cpp turns tool schemas into a
// sampling grammar and rejects some patterns (one inside anyOf inside array
// items: tempo_set's bpm) with HTTP 400 "failed to parse grammar"; the
// commands check quantity strings themselves, so the patterns add nothing.
json::Value withoutPatterns(const json::Value& schema) {
    if (schema.isArray()) {
        json::Value out = json::Value::array();
        for (const json::Value& v : schema.asArray()) out.push_back(withoutPatterns(v));
        return out;
    }
    if (!schema.isObject()) return schema;
    json::Value out = json::Value::object();
    for (const auto& m : schema.asObject())
        if (m.key != "pattern") out[m.key] = withoutPatterns(m.value);
    return out;
}
}  // namespace

json::Value openaiBody(const Endpoint& e, const Request& r) {
    json::Value body = json::Value::object();
    body["model"] = e.model;
    body["max_tokens"] = e.maxTokens;
    body["stream"] = true;
    json::Value messages = json::Value::array();
    if (!r.system.empty()) {
        json::Value sys = json::Value::object();
        sys["role"] = "system";
        sys["content"] = r.system;
        messages.push_back(std::move(sys));
    }
    for (const Message& m : r.messages) {
        for (const ToolResult& t : m.toolResults) {  // one "tool" message per result
            json::Value tool = json::Value::object();
            tool["role"] = "tool";
            tool["tool_call_id"] = t.id;
            tool["content"] = t.isError ? "error: " + t.content : t.content;
            messages.push_back(std::move(tool));
        }
        if (m.role == Message::Role::User && m.text.empty()) continue;
        json::Value msg = json::Value::object();
        msg["role"] = m.role == Message::Role::User ? "user" : "assistant";
        msg["content"] = m.text;
        if (!m.toolCalls.empty()) {
            json::Value calls = json::Value::array();
            for (const ToolCall& c : m.toolCalls) {
                json::Value call = json::Value::object();
                call["id"] = c.id;
                call["type"] = "function";
                json::Value fn = json::Value::object();
                fn["name"] = c.name;
                fn["arguments"] = json::writeCompact(c.input);
                call["function"] = std::move(fn);
                calls.push_back(std::move(call));
            }
            msg["tool_calls"] = std::move(calls);
        }
        messages.push_back(std::move(msg));
    }
    body["messages"] = std::move(messages);
    if (!r.tools.empty()) {
        json::Value tools = json::Value::array();
        for (const ToolDef& t : r.tools) {
            json::Value tool = json::Value::object();
            tool["type"] = "function";
            json::Value fn = json::Value::object();
            fn["name"] = t.name;
            fn["description"] = t.description;
            fn["parameters"] = withoutPatterns(t.inputSchema);
            tool["function"] = std::move(fn);
            tools.push_back(std::move(tool));
        }
        body["tools"] = std::move(tools);
    }
    return body;
}

namespace {

Result<Turn> openai(const Endpoint& e, const Request& r,
                    const std::function<void(std::string_view)>& onText, std::stop_token stop) {
    Turn turn;
    turn.message.role = Message::Role::Assistant;
    std::map<int, std::string> args;  // per tool call index
    std::map<int, ToolCall> toolCalls;
    bool done = false;  // data: [DONE]
    std::optional<Error> streamError;
    auto fail = [&](Error error) {
        if (!streamError) streamError = std::move(error);
    };
    // Optional fields are absent or null, or have their type.
    auto optional = [](const json::Value& v, bool (json::Value::*is)() const) {
        return v.isNull() || (v.*is)();
    };
    SseParser sse;
    auto onEvent = [&](std::string_view, std::string_view data) {
        if (streamError || done) return;
        if (data == "[DONE]") {
            done = true;
            return;
        }
        auto parsed = json::parse(data);
        if (!parsed || !parsed->isObject()) return fail(malformed("expected a JSON object"));
        const json::Value& ev = *parsed;
        if (const json::Value& err = at(ev, "error"); !err.isNull())
            return fail(Error("agent.api", at(err, "message").isString()
                                               ? at(err, "message").asString()
                                               : json::writeCompact(err)));
        const json::Value& usage = at(ev, "usage");
        if (!optional(usage, &json::Value::isObject))
            return fail(malformed("usage must be an object"));
        tokens(at(usage, "prompt_tokens"), turn.inputTokens);
        tokens(at(usage, "completion_tokens"), turn.outputTokens);
        const json::Value& choices = at(ev, "choices");
        if (!optional(choices, &json::Value::isArray))
            return fail(malformed("choices must be an array"));
        if (choices.isNull() || choices.asArray().empty()) return;  // e.g. the usage chunk
        const json::Value& choice = choices.asArray().front();
        if (!choice.isObject()) return fail(malformed("a choice must be an object"));
        const json::Value& fr = at(choice, "finish_reason");
        if (!optional(fr, &json::Value::isString))
            return fail(malformed("finish_reason must be a string or null"));
        if (fr.isString())
            turn.stopReason = fr.asString() == "tool_calls" ? "tool_use"
                              : fr.asString() == "stop"     ? "end_turn"
                              : fr.asString() == "length"   ? "max_tokens"
                                                            : fr.asString();
        const json::Value& delta = at(choice, "delta");
        if (!optional(delta, &json::Value::isObject))
            return fail(malformed("delta must be an object"));
        const json::Value& content = at(delta, "content");
        if (!optional(content, &json::Value::isString))
            return fail(malformed("delta content must be a string or null"));
        if (content.isString() && !content.asString().empty()) {
            turn.message.text += content.asString();
            if (onText) onText(content.asString());
        }
        const json::Value& calls = at(delta, "tool_calls");
        if (!optional(calls, &json::Value::isArray))
            return fail(malformed("tool_calls must be an array"));
        if (calls.isNull()) return;
        for (const json::Value& call : calls.asArray()) {
            if (!call.isObject()) return fail(malformed("a tool call must be an object"));
            int index = 0;
            if (const json::Value& value = at(call, "index"); !value.isNull()) {
                const auto checked = intIndex(value);
                if (!checked)
                    return fail(Error("agent.tool_index",
                                      "tool call index must be a nonnegative integer in range"));
                index = *checked;
            }
            const json::Value &id = at(call, "id"), &fn = at(call, "function");
            if (!optional(id, &json::Value::isString) || !optional(fn, &json::Value::isObject))
                return fail(malformed("a tool call's id is a string and its function an object"));
            const json::Value &name = at(fn, "name"), &arguments = at(fn, "arguments");
            if (!optional(name, &json::Value::isString) ||
                !optional(arguments, &json::Value::isString))
                return fail(malformed("a tool call's name and arguments are strings"));
            ToolCall& tc = toolCalls[index];
            if (id.isString()) tc.id = id.asString();
            if (name.isString()) tc.name += name.asString();
            if (arguments.isString()) args[index] += arguments.asString();
        }
    };
    std::vector<std::string> headers = {"content-type: application/json",
                                        "accept: text/event-stream"};
    if (!e.apiKey.empty()) headers.push_back("authorization: Bearer " + e.apiKey);
    auto status = post(
        trimSlash(e.baseUrl) + "/chat/completions", headers, json::writeCompact(openaiBody(e, r)),
        [&](std::string_view bytes) -> Status {
            if (auto fed = sse.feed(bytes, onEvent); !fed) return fed;
            if (streamError) return *streamError;  // stop reading
            return {};
        },
        stop);
    if (!status) return std::move(status).error();
    if (streamError) return *streamError;
    if (!done) return incomplete("no data: [DONE]");
    for (auto& [index, tc] : toolCalls) {
        if (tc.name.empty())
            return incomplete("tool call " + std::to_string(index) + " has no function name");
        auto input = parseObject(args[index]);
        if (!input) return input.error();
        tc.input = std::move(*input);
        if (tc.id.empty()) tc.id = "call_" + std::to_string(index);
        turn.message.toolCalls.push_back(std::move(tc));
    }
    if (turn.stopReason.empty())
        turn.stopReason = turn.message.toolCalls.empty() ? "end_turn" : "tool_use";
    return turn;
}

}  // namespace

Result<Turn> complete(const Endpoint& endpoint, const Request& request,
                      const std::function<void(std::string_view)>& onText, std::stop_token stop) {
    if (endpoint.baseUrl.empty() || endpoint.model.empty())
        return Error("agent.endpoint", "the endpoint needs a base URL and a model", "",
                     "set them in the agent panel's settings");
    return endpoint.kind == Endpoint::Kind::Anthropic ? anthropic(endpoint, request, onText, stop)
                                                      : openai(endpoint, request, onText, stop);
}

}  // namespace supple::agent
