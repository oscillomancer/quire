// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A scripted streaming LLM server for tests (Anthropic and OpenAI formats).
#pragma once

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "supple/agent/llm.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::agent::testing {

// Serves one scripted response per request (HTTP/1.1, Connection: close).
class FakeServer {
  public:
    struct Reply {
        int status = 200;
        std::vector<std::string> chunks;  // written with small pauses (streaming)
        int delayMs = 0;
        std::function<void()> beforeSend = {};
    };
    FakeServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        socklen_t len = sizeof addr;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        REQUIRE(::listen(fd_, 4) == 0);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeServer() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        thread_.join();
    }
    std::string url(const char* path = "") const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    void push(Reply r) {
        std::lock_guard lock(m_);
        replies_.push_back(std::move(r));
    }
    std::vector<json::Value> requests() {
        std::lock_guard lock(m_);
        return requests_;
    }

  private:
    void serve() {
        while (!stop_) {
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) return;
            std::string in;
            char buf[8192];
            std::size_t need = std::string::npos;
            while (true) {
                const auto n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                in.append(buf, static_cast<std::size_t>(n));
                const auto end = in.find("\r\n\r\n");
                if (end != std::string::npos && need == std::string::npos) {
                    const auto cl = in.find("ontent-Length: ");
                    need = end + 4 + (cl != std::string::npos ? std::stoul(in.substr(cl + 15)) : 0);
                }
                if (need != std::string::npos && in.size() >= need) break;
            }
            Reply r;
            {
                std::lock_guard lock(m_);
                const auto body = in.find("\r\n\r\n");
                if (body != std::string::npos)
                    if (auto j = json::parse(in.substr(body + 4))) requests_.push_back(*j);
                if (!replies_.empty()) {
                    r = std::move(replies_.front());
                    replies_.pop_front();
                } else {
                    r.status = 500;
                }
            }
            std::string head = "HTTP/1.1 " + std::to_string(r.status) +
                               (r.status == 200 ? " OK" : " Error") +
                               "\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
            ::send(c, head.data(), head.size(), MSG_NOSIGNAL);
            if (r.beforeSend) r.beforeSend();
            if (r.status != 200) {
                const std::string err = R"({"error": {"message": "invalid key"}})";
                ::send(c, err.data(), err.size(), MSG_NOSIGNAL);
            }
            for (const std::string& chunk : r.chunks) {
                if (r.delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(r.delayMs));
                if (::send(c, chunk.data(), chunk.size(), MSG_NOSIGNAL) < 0) break;
            }
            ::close(c);
        }
    }
    int fd_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex m_;
    std::deque<Reply> replies_;
    std::vector<json::Value> requests_;
};

inline const json::Value& el(const json::Value& v, std::size_t i) { return v.asArray().at(i); }

inline std::string sse(const std::string& event, const std::string& data) {
    return (event.empty() ? "" : "event: " + event + "\n") + "data: " + data + "\n\n";
}

// An Anthropic turn: optional text, then optional tool calls.
inline FakeServer::Reply anthropicTurn(const std::string& text,
                                       std::vector<std::pair<std::string, std::string>> calls) {
    FakeServer::Reply r;
    r.chunks.push_back(sse("message_start",
                           R"({"type":"message_start","message":{"usage":{"input_tokens":120}}})"));
    int index = 0;
    if (!text.empty()) {
        r.chunks.push_back(sse(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"));
        // Split the text across two deltas (and the second SSE event across two writes).
        const std::string a = text.substr(0, text.size() / 2), b = text.substr(text.size() / 2);
        r.chunks.push_back(
            sse("content_block_delta",
                R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":)" +
                    json::writeCompact(json::Value(a)) + "}}"));
        const std::string second =
            sse("content_block_delta",
                R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":)" +
                    json::writeCompact(json::Value(b)) + "}}");
        r.chunks.push_back(second.substr(0, 20));
        r.chunks.push_back(second.substr(20));
        r.chunks.push_back(sse("content_block_stop", R"({"type":"content_block_stop","index":0})"));
        index = 1;
    }
    for (const auto& [name, input] : calls) {
        const std::string i = std::to_string(index);
        r.chunks.push_back(
            sse("content_block_start", R"({"type":"content_block_start","index":)" + i +
                                           R"(,"content_block":{"type":"tool_use","id":"toolu_)" +
                                           i + R"(","name":")" + name + R"(","input":{}}})"));
        const std::string half = input.substr(0, input.size() / 2),
                          rest = input.substr(input.size() / 2);
        for (const std::string& part : {half, rest})
            r.chunks.push_back(sse("content_block_delta",
                                   R"({"type":"content_block_delta","index":)" + i +
                                       R"(,"delta":{"type":"input_json_delta","partial_json":)" +
                                       json::writeCompact(json::Value(part)) + "}}"));
        r.chunks.push_back(
            sse("content_block_stop", R"({"type":"content_block_stop","index":)" + i + "}"));
        ++index;
    }
    r.chunks.push_back(
        sse("message_delta", std::string(R"({"type":"message_delta","delta":{"stop_reason":")") +
                                 (calls.empty() ? "end_turn" : "tool_use") +
                                 R"("},"usage":{"output_tokens":42}})"));
    r.chunks.push_back(sse("message_stop", R"({"type":"message_stop"})"));
    return r;
}

inline FakeServer::Reply openaiTurn(const std::string& text,
                                    std::vector<std::pair<std::string, std::string>> calls) {
    FakeServer::Reply r;
    if (!text.empty())
        for (const std::string& part : {text.substr(0, 3), text.substr(3)})
            r.chunks.push_back(sse("", R"({"choices":[{"index":0,"delta":{"content":)" +
                                           json::writeCompact(json::Value(part)) + "}}]}"));
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto& [name, input] = calls[i];
        const std::string idx = std::to_string(i);
        r.chunks.push_back(sse("", R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":)" +
                                       idx + R"(,"id":"call_)" + idx +
                                       R"(","type":"function","function":{"name":")" + name +
                                       R"(","arguments":""}}]}}]})"));
        for (const std::string& part : {input.substr(0, 5), input.substr(5)})
            r.chunks.push_back(
                sse("", R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":)" + idx +
                            R"(,"function":{"arguments":)" + json::writeCompact(json::Value(part)) +
                            "}}]}}]}"));
    }
    r.chunks.push_back(
        sse("", std::string(R"({"choices":[{"index":0,"delta":{},"finish_reason":")") +
                    (calls.empty() ? "stop" : "tool_calls") +
                    R"("}],"usage":{"prompt_tokens":99,"completion_tokens":7}})"));
    r.chunks.push_back("data: [DONE]\n\n");
    return r;
}

}  // namespace supple::agent::testing
