// The agent session against a scripted fake LLM server (Anthropic and
// OpenAI streaming formats), with a real engine and bus.
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "supple/agent/llm.h"
#include "supple/agent/session.h"
#include "supple/engine/app.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

using namespace supple;
using namespace supple::agent;

namespace {

// Serves one scripted response per request (HTTP/1.1, Connection: close).
class FakeServer {
  public:
    struct Reply {
        int status = 200;
        std::vector<std::string> chunks;  // written with small pauses (streaming)
        int delayMs = 0;
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

const json::Value& el(const json::Value& v, std::size_t i) { return v.asArray().at(i); }

std::string sse(const std::string& event, const std::string& data) {
    return (event.empty() ? "" : "event: " + event + "\n") + "data: " + data + "\n\n";
}

// An Anthropic turn: optional text, then optional tool calls.
FakeServer::Reply anthropicTurn(const std::string& text,
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

FakeServer::Reply openaiTurn(const std::string& text,
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

struct Rig {
    std::filesystem::path dir;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::mutex m;
    std::vector<std::function<void()>> posted;
    Rig() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-agent-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        engine::AppOptions options;
        options.workdir = dir;
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
    }
    ~Rig() {
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    Session::Post post() {
        return [this](std::function<void()> f) {
            std::lock_guard lock(m);
            posted.push_back(std::move(f));
        };
    }
    // The model thread: run posted work until `done` or a timeout.
    bool pump(const std::function<bool()>& done, int ms = 10000) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done() && std::chrono::steady_clock::now() < until) {
            std::vector<std::function<void()>> now;
            {
                std::lock_guard lock(m);
                now.swap(posted);
            }
            for (auto& f : now) f();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }
    std::size_t tracks(std::string_view name) {
        std::size_t n = 0;
        for (const auto& e : app->head().tracks) n += e.value->name == name;
        return n;
    }
};

Options skillOptions(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir / "guides");
    std::ofstream(dir / "SKILL.md")
        << "---\nname: test\n---\n# Test skill\nMeasure, don't guess.\n";
    std::ofstream(dir / "guides" / "mixing.md") << "# Mixing\nGain stage first.\n";
    Options o;
    o.skillDir = dir;
    return o;
}

}  // namespace

TEST_CASE("SSE parsing survives arbitrary chunking") {
    SseParser p;
    std::vector<std::pair<std::string, std::string>> got;
    const std::string stream =
        "event: a\ndata: {\"x\":1}\n\n: comment\ndata: line1\ndata: line2\r\n\r\ndata: [DONE]\n\n";
    for (char c : stream)
        p.feed(std::string_view(&c, 1),
               [&](std::string_view e, std::string_view d) { got.emplace_back(e, d); });
    REQUIRE(got.size() == 3);
    CHECK(got[0] == std::pair<std::string, std::string>{"a", "{\"x\":1}"});
    CHECK(got[1].second == "line1\nline2");
    CHECK(got[2].second == "[DONE]");
}

TEST_CASE("request bodies carry history, tool calls and results in each format") {
    Endpoint e{Endpoint::Kind::Anthropic, "t", "http://x", "m", "k", 100};
    Request r;
    r.system = "sys";
    r.messages.push_back({Message::Role::User, "hi", {}, {}});
    r.messages.push_back(
        {Message::Role::Assistant, "ok", {{"c1", "exec", *json::parse(R"({"command":"x"})")}}, {}});
    r.messages.push_back({Message::Role::User, "", {}, {{"c1", "{\"done\":true}", false}}});
    r.tools.push_back({"exec", "Run", *json::parse(R"({"type":"object"})")});
    auto a = anthropicBody(e, r);
    CHECK(a["system"].asString() == "sys");
    CHECK(a["messages"].asArray().size() == 3);
    CHECK(at(el(at(el(at(a, "messages"), 1), "content"), 1), "type").asString() == "tool_use");
    CHECK(at(el(at(el(at(a, "messages"), 2), "content"), 0), "tool_use_id").asString() == "c1");
    CHECK(at(at(el(at(a, "tools"), 0), "input_schema"), "type").asString() == "object");
    auto o = openaiBody(e, r);
    CHECK(at(el(at(o, "messages"), 0), "role").asString() == "system");
    CHECK(at(at(el(at(el(at(o, "messages"), 2), "tool_calls"), 0), "function"), "arguments")
              .asString() == R"({"command":"x"})");
    CHECK(at(el(at(o, "messages"), 3), "role").asString() == "tool");
    CHECK(at(at(at(el(at(o, "tools"), 0), "function"), "parameters"), "type").asString() ==
          "object");
}

TEST_CASE("an Anthropic agent edits on its own branch; the human diffs, A/Bs and accepts") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn(
        "I'll add a drum buss.",
        {{"exec", R"({"command": "track.add", "args": {"type": "buss", "name": "Drums"}})"}}));
    server.push(anthropicTurn("Added a buss named Drums.", {}));
    Session s({Endpoint::Kind::Anthropic, "fake", server.url(), "test-model", "sk-test"},
              rig.registry, *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    std::string streamed;
    int started = 0, finished = 0;
    bool idle = false;
    s.events.text = [&](std::string_view d) { streamed += d; };
    s.events.toolStarted = [&](const ToolCall&) { ++started; };
    s.events.toolFinished = [&](const ToolCall&, const ToolResult& r) {
        ++finished;
        CHECK_FALSE(r.isError);
    };
    s.events.idle = [&] { idle = true; };
    s.events.error = [&](const std::string& e) { FAIL(e); };
    REQUIRE(s.send("Add a drum buss."));
    CHECK(s.branch() == "agent/panel/1");
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK(streamed == "I'll add a drum buss.Added a buss named Drums.");
    CHECK(started == 1);
    CHECK(finished == 1);
    CHECK(rig.tracks("Drums") == 1);  // on the agent's branch (current)

    // What the server saw: the skill as system prompt, tools, and the tool result.
    auto reqs = server.requests();
    REQUIRE(reqs.size() == 2);
    CHECK(at(reqs[0], "system").asString().find("Measure, don't guess.") != std::string::npos);
    CHECK(at(reqs[0], "system").asString().find("name: test") ==
          std::string::npos);  // front matter stripped
    bool hasSkillTool = false;
    for (const auto& t : at(reqs[0], "tools").asArray())
        hasSkillTool |= at(t, "name").asString() == "skill_read";
    CHECK(hasSkillTool);
    CHECK(at(reqs[1], "messages").asArray().size() == 3);
    CHECK(at(el(at(el(at(reqs[1], "messages"), 2), "content"), 0), "type").asString() ==
          "tool_result");

    // The change is attributed to the agent.
    auto history = rig.dispatcher->execute({"history.list", json::Value::object(), {}});
    REQUIRE(history);
    CHECK(
        at(el(at((*history), "versions"), 0), "origin").asString().find("agent:panel/test-model") ==
        0);

    // Diff, A/B, accept.
    auto diff = s.diff();
    REQUIRE(diff);
    // Ids and fields; the panel resolves names from the two heads.
    CHECK(json::writeCompact(*diff).find(R"({"op":"add","id":"tr_1")") != std::string::npos);
    REQUIRE(s.audition(false));
    CHECK(rig.tracks("Drums") == 0);  // the human's version
    REQUIRE(s.audition(true));
    CHECK(rig.tracks("Drums") == 1);
    REQUIRE(s.accept());
    CHECK(rig.tracks("Drums") == 1);  // merged into the base, which is current again
    CHECK(s.branch().empty());
    auto list = rig.dispatcher->execute({"branch.list", json::Value::object(), {}});
    CHECK(json::writeCompact(*list).find("agent/panel/1") == std::string::npos);
}

TEST_CASE("an OpenAI-compatible agent: tool calls stream in pieces; reject discards the branch") {
    Rig rig;
    FakeServer server;
    server.push(
        openaiTurn("On it.", {{"exec", R"({"command": "track.add", "args": {"name": "Pad"}})"}}));
    server.push(openaiTurn("Done.", {}));
    Session s({Endpoint::Kind::OpenAI, "local", server.url("/v1"), "llama", ""}, rig.registry,
              *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    bool idle = false;
    std::vector<Turn> turns;
    s.events.idle = [&] { idle = true; };
    s.events.turn = [&](const Turn& t) { turns.push_back(t); };
    s.events.error = [&](const std::string& e) { FAIL(e); };
    REQUIRE(s.send("Add a pad track."));
    REQUIRE(rig.pump([&] { return idle; }));
    REQUIRE(turns.size() == 2);
    CHECK(turns[0].stopReason == "tool_use");
    CHECK(at(at(turns[0].message.toolCalls.at(0).input, "args"), "name").asString() == "Pad");
    CHECK(turns[1].inputTokens == 99);
    CHECK(rig.tracks("Pad") == 1);
    auto reqs = server.requests();
    REQUIRE(reqs.size() == 2);
    CHECK(at(at(reqs[1], "messages").asArray().back(), "role").asString() == "tool");
    REQUIRE(s.reject());
    CHECK(rig.tracks("Pad") == 0);
}

TEST_CASE(
    "listen cards stop the turn; skill topics load on demand; errors and cancel are reported") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn("", {{"skill_read", R"({"topic": "guides/mixing.md"})"}}));
    server.push(anthropicTurn(
        "Please listen.",
        {{"listen",
          R"({"what": "is the kick too loud?", "start_bar": 1, "end_bar": 4, "ab": true})"}}));
    Session s({Endpoint::Kind::Anthropic, "fake", server.url(), "m", "k"}, rig.registry,
              *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    bool idle = false;
    json::Value card;
    std::string skillText;
    s.events.idle = [&] { idle = true; };
    s.events.listen = [&](const json::Value& c) { card = c; };
    s.events.toolFinished = [&](const ToolCall& c, const ToolResult& r) {
        if (c.name == "skill_read") skillText = r.content;
    };
    REQUIRE(s.send("Check the kick."));
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK(skillText.find("Gain stage first.") != std::string::npos);
    CHECK(at(card, "what").asString() == "is the kick too loud?");
    CHECK(server.requests().size() == 2);  // stopped after the listen card

    // HTTP errors become an error event with a hint.
    server.push({401, {}, 0});
    idle = false;
    std::string error;
    s.events.error = [&](const std::string& e) { error = e; };
    REQUIRE(s.send("again"));
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK(error.find("401") != std::string::npos);

    // Cancel during a slow stream.
    auto slow = anthropicTurn("This is a very slow answer.", {});
    slow.delayMs = 300;
    server.push(slow);
    idle = false;
    error.clear();
    REQUIRE(s.send("slow"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    s.cancel();
    REQUIRE(rig.pump([&] { return idle; }, 5000));
    CHECK(error.find("cancel") != std::string::npos);
}
