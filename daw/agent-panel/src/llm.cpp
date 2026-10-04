#include "supple/agent/llm.h"

#include <curl/curl.h>

#include <cmath>
#include <limits>
#include <map>
#include <mutex>

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
    if (raw.empty()) return json::Value::object();
    auto v = json::parse(raw);
    if (!v)
        return Error("agent.tool_arguments", "invalid tool argument JSON", {},
                     v.error().toString());
    if (!v->isObject()) return Error("agent.tool_arguments", "tool arguments must be an object");
    return std::move(*v);
}

std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// --- streaming POST

struct Stream {
    std::function<void(std::string_view)> onBytes;
    std::stop_token stop;
    std::string head;  // the first bytes, kept for error messages
    long status = 0;
};

size_t onWrite(char* data, size_t size, size_t count, void* user) {
    auto* s = static_cast<Stream*>(user);
    const size_t n = size * count;
    if (s->stop.stop_requested()) return 0;  // aborts the transfer
    if (s->head.size() < 2048) s->head.append(data, std::min(n, 2048 - s->head.size()));
    s->onBytes(std::string_view(data, n));
    return n;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Stream*>(user)->stop.stop_requested() ? 1 : 0;
}

Status post(const std::string& url, const std::vector<std::string>& headers,
            const std::string& body, const std::function<void(std::string_view)>& onBytes,
            std::stop_token stop) {
    initCurl();
    CURL* curl = curl_easy_init();
    if (!curl) return Error("agent.http", "could not start an HTTP client");
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    Stream s{onBytes, stop, {}, 0};
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
    if (rc != CURLE_OK)
        return Error("agent.http", std::string("request failed: ") + curl_easy_strerror(rc), url);
    if (s.status < 200 || s.status >= 300)
        return Error("agent.http", "HTTP " + std::to_string(s.status) + ": " + s.head, url,
                     s.status == 401 || s.status == 403 ? "check the API key" : "");
    return {};
}

}  // namespace

// --- SSE

void SseParser::feed(std::string_view bytes,
                     const std::function<void(std::string_view, std::string_view)>& out) {
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
        } else if (line.rfind("event:", 0) == 0) {
            std::string_view v = line.substr(6);
            if (!v.empty() && v.front() == ' ') v.remove_prefix(1);
            event_ = std::string(v);
        }  // comments (":") and other fields are ignored
    }
    buffer_.erase(0, start);
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
    std::map<int, std::string> partialJson;  // per tool_use block index
    std::map<int, std::size_t> callAt;
    std::optional<Error> streamError;
    SseParser sse;
    auto onEvent = [&](std::string_view, std::string_view data) {
        auto ev = json::parse(data);
        if (!ev || !ev->isObject()) return;
        const std::string type = ev->find("type") ? at((*ev), "type").asString() : "";
        if (type == "content_block_start") {
            const json::Value& block = at((*ev), "content_block");
            if (at(block, "type").asString() == "tool_use") {
                const int index = static_cast<int>(at((*ev), "index").asInt());
                callAt[index] = turn.message.toolCalls.size();
                turn.message.toolCalls.push_back(
                    {at(block, "id").asString(), at(block, "name").asString(), {}});
            }
        } else if (type == "content_block_delta") {
            const json::Value& delta = at((*ev), "delta");
            const std::string dt = at(delta, "type").asString();
            if (dt == "text_delta") {
                const std::string& t = at(delta, "text").asString();
                turn.message.text += t;
                if (onText) onText(t);
            } else if (dt == "input_json_delta") {
                partialJson[static_cast<int>(at((*ev), "index").asInt())] +=
                    at(delta, "partial_json").asString();
            }
        } else if (type == "content_block_stop") {
            const int index = static_cast<int>(at((*ev), "index").asInt());
            if (auto it = callAt.find(index); it != callAt.end()) {
                auto input = parseObject(partialJson[index]);
                if (!input)
                    streamError = input.error();
                else
                    turn.message.toolCalls[it->second].input = std::move(*input);
            }
        } else if (type == "message_start") {
            if (const json::Value* u = at((*ev), "message").find("usage"))
                if (const json::Value* in = u->find("input_tokens"))
                    turn.inputTokens = static_cast<int>(in->asInt());
        } else if (type == "message_delta") {
            if (const json::Value* d = ev->find("delta");
                d && d->find("stop_reason") && at((*d), "stop_reason").isString())
                turn.stopReason = at((*d), "stop_reason").asString();
            if (const json::Value* u = ev->find("usage"))
                if (const json::Value* out = u->find("output_tokens"))
                    turn.outputTokens = static_cast<int>(out->asInt());
        } else if (type == "error") {
            streamError = Error("agent.api", at(at((*ev), "error"), "message").asString());
        }
    };
    std::vector<std::string> headers = {"content-type: application/json",
                                        "anthropic-version: 2023-06-01",
                                        "accept: text/event-stream"};
    if (!e.apiKey.empty()) headers.push_back("x-api-key: " + e.apiKey);
    auto status = post(
        trimSlash(e.baseUrl) + "/v1/messages", headers, json::writeCompact(anthropicBody(e, r)),
        [&](std::string_view bytes) { sse.feed(bytes, onEvent); }, stop);
    if (!status) return std::move(status).error();
    if (streamError) return *streamError;
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
    std::optional<Error> streamError;
    SseParser sse;
    auto onEvent = [&](std::string_view, std::string_view data) {
        if (data == "[DONE]") return;
        auto ev = json::parse(data);
        if (!ev || !ev->isObject()) return;
        if (const json::Value* err = ev->find("error")) {
            streamError = Error("agent.api", err->isObject() && err->find("message")
                                                 ? at((*err), "message").asString()
                                                 : json::writeCompact(*err));
            return;
        }
        if (const json::Value* usage = ev->find("usage"); usage && usage->isObject()) {
            if (const json::Value* p = usage->find("prompt_tokens"); p && p->isNumber())
                turn.inputTokens = static_cast<int>(p->asInt());
            if (const json::Value* c = usage->find("completion_tokens"); c && c->isNumber())
                turn.outputTokens = static_cast<int>(c->asInt());
        }
        const json::Value* choices = ev->find("choices");
        if (!choices || !choices->isArray() || choices->asArray().empty()) return;
        const json::Value& choice = choices->asArray().front();
        if (const json::Value* fr = choice.find("finish_reason"); fr && fr->isString())
            turn.stopReason = fr->asString() == "tool_calls" ? "tool_use"
                              : fr->asString() == "stop"     ? "end_turn"
                              : fr->asString() == "length"   ? "max_tokens"
                                                             : fr->asString();
        const json::Value* delta = choice.find("delta");
        if (!delta || !delta->isObject()) return;
        if (const json::Value* c = delta->find("content");
            c && c->isString() && !c->asString().empty()) {
            turn.message.text += c->asString();
            if (onText) onText(c->asString());
        }
        if (const json::Value* calls = delta->find("tool_calls"); calls && calls->isArray())
            for (const json::Value& call : calls->asArray()) {
                int index = 0;
                if (const auto* value = call.find("index")) {
                    if (!value->isNumber() || !std::isfinite(value->asDouble()) ||
                        value->asDouble() < 0 ||
                        value->asDouble() > std::numeric_limits<int>::max() ||
                        std::floor(value->asDouble()) != value->asDouble()) {
                        streamError =
                            Error("agent.tool_index",
                                  "tool call index must be a nonnegative integer in range");
                        return;
                    }
                    index = static_cast<int>(value->asDouble());
                }
                ToolCall& tc = toolCalls[index];
                if (const json::Value* id = call.find("id"); id && id->isString())
                    tc.id = id->asString();
                if (const json::Value* fn = call.find("function"); fn && fn->isObject()) {
                    if (const json::Value* n = fn->find("name"); n && n->isString())
                        tc.name += n->asString();
                    if (const json::Value* a = fn->find("arguments"); a && a->isString())
                        args[index] += a->asString();
                }
            }
    };
    std::vector<std::string> headers = {"content-type: application/json",
                                        "accept: text/event-stream"};
    if (!e.apiKey.empty()) headers.push_back("authorization: Bearer " + e.apiKey);
    auto status = post(
        trimSlash(e.baseUrl) + "/chat/completions", headers, json::writeCompact(openaiBody(e, r)),
        [&](std::string_view bytes) { sse.feed(bytes, onEvent); }, stop);
    if (!status) return std::move(status).error();
    if (streamError) return *streamError;
    for (auto& [index, tc] : toolCalls) {
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
