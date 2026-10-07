// The agent session against a scripted fake LLM server (Anthropic and
// OpenAI streaming formats), with a real engine and bus.
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "fake_llm.h"
#include "supple/agent/config.h"
#include "supple/agent/llm.h"
#include "supple/agent/session.h"
#include "supple/engine/app.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

using namespace supple;
using namespace supple::agent;
using namespace supple::agent::testing;

namespace {

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
    // Waits, running nothing, until `n` closures are posted (the worker's
    // handshake: the model thread has not run them yet).
    bool waitPosted(std::size_t n, int ms = 10000) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            {
                std::lock_guard lock(m);
                if (posted.size() >= n) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    std::string branch() {
        auto project = dispatcher->execute({"project.get", json::Value::object(), {}});
        REQUIRE(project);
        return at(*project, "branch").asString();
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

TEST_CASE("follow-up agent turns return to their branch after auditioning the base") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn("First turn", {}));
    server.push(anthropicTurn(
        "Follow-up", {{"exec", R"({"command":"track.add","args":{"name":"Follow-up"}})"}}));
    server.push(anthropicTurn("Done", {}));
    Session s({Endpoint::Kind::Anthropic, "fake", server.url(), "test-model", "sk-test"},
              rig.registry, *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    bool idle = false;
    s.events.idle = [&] { idle = true; };
    s.events.error = [&](const std::string& error) { FAIL(error); };
    REQUIRE(s.send("Start"));
    REQUIRE(rig.pump([&] { return idle; }));
    REQUIRE(s.audition(false));
    idle = false;
    REQUIRE(s.send("Add a track"));
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK(rig.tracks("Follow-up") == 1);
    REQUIRE(s.audition(false));
    CHECK(rig.tracks("Follow-up") == 0);
    REQUIRE(s.reject());
}

TEST_CASE("SSE parsing survives arbitrary chunking") {
    SseParser p;
    std::vector<std::pair<std::string, std::string>> got;
    const std::string stream =
        "event: a\ndata: {\"x\":1}\n\n: comment\ndata: line1\ndata: line2\r\n\r\ndata: [DONE]\n\n";
    for (char c : stream)
        REQUIRE(p.feed(std::string_view(&c, 1),
                       [&](std::string_view e, std::string_view d) { got.emplace_back(e, d); }));
    REQUIRE(got.size() == 3);
    CHECK(got[0] == std::pair<std::string, std::string>{"a", "{\"x\":1}"});
    CHECK(got[1].second == "line1\nline2");
    CHECK(got[2].second == "[DONE]");
}

TEST_CASE("streamed tool calls reject malformed arguments and invalid indices") {
    Request request;
    request.messages.push_back({Message::Role::User, "Test", {}, {}});
    for (const auto kind : {Endpoint::Kind::Anthropic, Endpoint::Kind::OpenAI}) {
        for (const auto* input : {"{broken", "[]   "}) {
            FakeServer server;
            server.push(kind == Endpoint::Kind::Anthropic ? anthropicTurn("", {{"exec", input}})
                                                          : openaiTurn("", {{"exec", input}}));
            auto turn =
                complete({kind, "fake", server.url(), "test-model", "sk-test"}, request, {}, {});
            REQUIRE_FALSE(turn);
            CHECK(turn.error().code == "agent.tool_arguments");
        }
    }
    for (const auto* index : {"-1", "0.5", "1e100", "2147483647"}) {
        FakeServer server;
        auto reply = openaiTurn("", {{"exec", "{}   "}});
        for (auto& chunk : reply.chunks) {
            const auto position = chunk.find("\"tool_calls\":[{\"index\":0");
            if (position != std::string::npos)
                chunk.replace(position + std::string("\"tool_calls\":[{\"index\":").size(), 1,
                              index);
        }
        server.push(std::move(reply));
        auto turn =
            complete({Endpoint::Kind::OpenAI, "fake", server.url(), "test-model", "sk-test"},
                     request, {}, {});
        if (std::string_view(index) == "2147483647") {
            REQUIRE(turn);
            CHECK(turn->message.toolCalls.size() == 1);
        } else {
            REQUIRE_FALSE(turn);
            CHECK(turn.error().code == "agent.tool_index");
        }
    }
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

TEST_CASE("OpenAI-compatible tool schemas leave out regex patterns (llama.cpp grammars)") {
    Endpoint e{Endpoint::Kind::OpenAI, "t", "http://x", "m", "k", 100};
    Request r;
    r.messages.push_back({Message::Role::User, "hi", {}, {}});
    r.tools.push_back({"tempo_set", "Set the tempo", *json::parse(R"({"type":"object",
        "properties":{"points":{"type":"array","items":{"type":"object","properties":{
        "bpm":{"anyOf":[{"type":"number"},{"type":"string","pattern":"^[0-9]+$"}]}}}}}})")});
    const std::string text = json::writeCompact(openaiBody(e, r));
    CHECK(text.find("pattern") == std::string::npos);
    CHECK(text.find("anyOf") != std::string::npos);
    // The Anthropic body keeps the schema as it is.
    CHECK(json::writeCompact(anthropicBody(e, r)).find("pattern") != std::string::npos);
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

    // Hold the response until cancellation: scheduling cannot finish it first.
    auto slow = anthropicTurn("This is a very slow answer.", {});
    std::promise<void> arrived, release;
    auto ready = arrived.get_future();
    auto gate = release.get_future().share();
    slow.beforeSend = [&arrived, gate] {
        arrived.set_value();
        gate.wait();
    };
    server.push(std::move(slow));
    idle = false;
    error.clear();
    REQUIRE(s.send("slow"));
    const bool reached = ready.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    s.cancel();
    release.set_value();
    REQUIRE(reached);
    REQUIRE(rig.pump([&] { return idle; }, 5000));
    CHECK(error.find("cancel") != std::string::npos);
}

namespace {

Request oneMessage() {
    Request request;
    request.messages.push_back({Message::Role::User, "Test", {}, {}});
    return request;
}

Result<Turn> streamed(Endpoint::Kind kind, std::vector<std::string> chunks) {
    FakeServer server;
    server.push({200, std::move(chunks), 0});
    return complete({kind, "fake", server.url(), "test-model", "sk-test"}, oneMessage(), {}, {});
}

// `reply` without the chunks that contain `drop`, or with `from` replaced by `to`.
std::vector<std::string> edited(const FakeServer::Reply& reply, std::string_view drop,
                                std::string_view from = {}, std::string_view to = {}) {
    std::vector<std::string> out;
    for (std::string chunk : reply.chunks) {
        if (!drop.empty() && chunk.find(drop) != std::string::npos) continue;
        if (!from.empty())
            if (auto at = chunk.find(from); at != std::string::npos)
                chunk.replace(at, from.size(), to);
        out.push_back(std::move(chunk));
    }
    return out;
}

std::string code(const Result<Turn>& turn) { return turn ? "ok" : turn.error().code; }

}  // namespace

TEST_CASE("HTTP error bodies obey the response size limit") {
    FakeServer server;
    FakeServer::Reply reply;
    reply.status = 500;
    const std::string chunk(4096, 'x');
    for (std::size_t bytes = 0; bytes <= kMaxResponseBytes; bytes += chunk.size())
        reply.chunks.push_back(chunk);
    server.push(std::move(reply));
    const auto result =
        complete({Endpoint::Kind::OpenAI, "fake", server.url(), "test-model", ""}, {}, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "agent.stream_too_large");
}

TEST_CASE("SSE events beyond the size limit fail instead of buffering") {
    SseParser p;
    auto ignore = [](std::string_view, std::string_view) {};
    const std::string big(kMaxEventBytes / 2 + 1, 'x');
    REQUIRE(p.feed("data: " + big + "\n", ignore));
    auto over = p.feed("data: " + big + "\n", ignore);  // one event across two lines
    REQUIRE_FALSE(over);
    CHECK(over.error().code == "agent.stream_too_large");
    SseParser line;
    auto unterminated = line.feed(std::string(kMaxEventBytes + 1, 'x'), ignore);
    REQUIRE_FALSE(unterminated);
    CHECK(unterminated.error().code == "agent.stream_too_large");
}

TEST_CASE("Anthropic streams must be typed and end with message_stop") {
    const auto tool = anthropicTurn("Hi", {{"exec", R"({"command":"track.list"})"}});
    REQUIRE(code(streamed(Endpoint::Kind::Anthropic, tool.chunks)) == "ok");
    // Cut off: no completion marker, or a tool block that never finished.
    CHECK(code(streamed(Endpoint::Kind::Anthropic, edited(tool, "message_stop"))) ==
          "agent.stream_incomplete");
    CHECK(code(streamed(Endpoint::Kind::Anthropic,
                        edited(tool, R"({"type":"content_block_stop","index":1})"))) ==
          "agent.stream_incomplete");
    // Mistyped fields are errors, not unchecked accesses.
    CHECK(code(streamed(Endpoint::Kind::Anthropic,
                        edited(tool, "", R"("index":1,)", R"("index":"1",)"))) == "agent.stream");
    CHECK(code(streamed(Endpoint::Kind::Anthropic, edited(tool, "", R"("text_delta","text":)",
                                                          R"("text_delta","text":5,"x":)"))) ==
          "agent.stream");
    CHECK(code(streamed(Endpoint::Kind::Anthropic, edited(tool, "", R"("id":"toolu_1",)", ""))) ==
          "agent.stream");
    CHECK(code(streamed(Endpoint::Kind::Anthropic, edited(tool, "", R"({"type":"message_start")",
                                                          "{not json"))) == "agent.stream");
    CHECK(code(streamed(Endpoint::Kind::Anthropic,
                        edited(tool, "", R"("stop_reason":"tool_use")", R"("stop_reason":7)"))) ==
          "agent.stream");
    // A delta for a block that was never started.
    CHECK(code(streamed(Endpoint::Kind::Anthropic, edited(tool, R"("index":1,"content_block")"))) ==
          "agent.stream");

    // Pings are ignored; a tool_use without input deltas keeps its start input.
    auto turn = streamed(
        Endpoint::Kind::Anthropic,
        {sse("message_start", R"({"type":"message_start","message":{}})"),
         sse("ping", R"({"type":"ping"})"),
         sse("content_block_start",
             R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"t","name":"exec","input":{"command":"track.list"}}})"),
         sse("content_block_stop", R"({"type":"content_block_stop","index":0})"),
         sse("message_delta", R"({"type":"message_delta","delta":{"stop_reason":null}})"),
         sse("message_stop", R"({"type":"message_stop"})")});
    REQUIRE(turn);
    REQUIRE(turn->message.toolCalls.size() == 1);
    CHECK(at(turn->message.toolCalls[0].input, "command").asString() == "track.list");
    CHECK(turn->stopReason == "tool_use");
}

TEST_CASE("OpenAI-compatible streams must be typed and end with [DONE]") {
    const auto tool = openaiTurn("Hello", {{"exec", R"({"command":"track.list"})"}});
    REQUIRE(code(streamed(Endpoint::Kind::OpenAI, tool.chunks)) == "ok");
    CHECK(code(streamed(Endpoint::Kind::OpenAI, edited(tool, "[DONE]"))) ==
          "agent.stream_incomplete");
    CHECK(code(streamed(Endpoint::Kind::OpenAI,
                        edited(tool, "", R"({"choices":[{"index":0,"delta":{"content")",
                               R"({"choices":"x","y":[{"index":0,"delta":{"content")"))) ==
          "agent.stream");
    CHECK(code(streamed(Endpoint::Kind::OpenAI, edited(tool, "", R"("delta":{"content":)",
                                                       R"("delta":{"content":5,"x":)"))) ==
          "agent.stream");
    CHECK(code(streamed(Endpoint::Kind::OpenAI, edited(tool, "", R"("function":{"arguments":)",
                                                       R"("function":{"arguments":5,"x":)"))) ==
          "agent.stream");
    CHECK(code(streamed(Endpoint::Kind::OpenAI,
                        edited(tool, "", R"({"choices":[{"index":0,"delta":{"content")",
                               "not json at all"))) == "agent.stream");
    // A tool call whose argument deltas never came, or whose name never came.
    CHECK(code(streamed(Endpoint::Kind::OpenAI, edited(tool, R"("function":{"arguments":)"))) ==
          "agent.tool_arguments");
    CHECK(code(streamed(Endpoint::Kind::OpenAI, edited(tool, R"("name":"exec")"))) ==
          "agent.stream_incomplete");

    // Nulls where OpenAI sends them are fine.
    auto turn = streamed(
        Endpoint::Kind::OpenAI,
        {sse("",
             R"({"choices":[{"index":0,"delta":{"role":"assistant","content":null,"tool_calls":null},"finish_reason":null}],"usage":null})"),
         sse("", R"({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})"),
         sse("", R"({"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":1}})"),
         "data: [DONE]\n\n"});
    REQUIRE(turn);
    CHECK(turn->message.text == "ok");
    CHECK(turn->stopReason == "end_turn");
    CHECK(turn->inputTokens == 3);
}

TEST_CASE("the endpoint config is replaced atomically and stays owner-only") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-agent-config-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const auto file = dir / "slug" / "agent.json";
    auto mode = [](const std::filesystem::path& p) {
        struct stat st{};
        REQUIRE(::stat(p.c_str(), &st) == 0);
        return st.st_mode & 0777;
    };
    auto onlyTheFile = [&] {
        std::vector<std::string> names;
        for (const auto& f : std::filesystem::directory_iterator(file.parent_path()))
            names.push_back(f.path().filename().string());
        return names == std::vector<std::string>{"agent.json"};
    };
    Config c;
    c.endpoints.push_back({Endpoint::Kind::OpenAI, "local", "http://127.0.0.1:1/v1", "m", "sk-1"});
    REQUIRE(c.save(file));
    CHECK(mode(file) == 0600);
    CHECK(onlyTheFile());

    // An existing file readable by others is replaced by an owner-only one.
    ::chmod(file.c_str(), 0644);
    c.endpoints[0].apiKey = "sk-2";
    REQUIRE(c.save(file));
    CHECK(mode(file) == 0600);
    CHECK(onlyTheFile());
    auto loaded = Config::load(file);
    REQUIRE(loaded);
    CHECK(loaded->endpoints.at(0).apiKey == "sk-2");

    // A failed write leaves the previous file as it was, and no temporary file.
    if (::geteuid() != 0) {
        ::chmod(file.parent_path().c_str(), 0500);
        c.endpoints[0].apiKey = "sk-3";
        auto denied = c.save(file);
        ::chmod(file.parent_path().c_str(), 0700);
        REQUIRE_FALSE(denied);
        CHECK(denied.error().code == "agent.config");
        CHECK(Config::load(file)->endpoints.at(0).apiKey == "sk-2");
        CHECK(onlyTheFile());
    }
    const auto blocked = dir / "slug" / "blocked";  // a non-empty directory: rename fails
    std::filesystem::create_directories(blocked / "inside");
    REQUIRE_FALSE(c.save(blocked));
    for (const auto& f : std::filesystem::directory_iterator(file.parent_path()))
        CHECK(f.path().filename().string().find(".blocked.") == std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("cancel invalidates a queued tool call; the panel is busy until the worker is done") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn(
        "", {{"exec", R"({"command": "track.add", "args": {"name": "Cancelled"}})"}}));
    server.push(anthropicTurn("Resumed.", {}));
    Session s({Endpoint::Kind::Anthropic, "fake", server.url(), "m", "k"}, rig.registry,
              *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    bool idle = false;
    int started = 0;
    std::string error;
    s.events.idle = [&] { idle = true; };
    s.events.toolStarted = [&](const ToolCall&) { ++started; };
    s.events.error = [&](const std::string& e) { error = e; };
    REQUIRE(s.send("Add a track."));
    // The worker has posted the turn event and the tool call; neither has run.
    REQUIRE(rig.waitPosted(2));
    s.cancel();
    CHECK(s.busy());  // the worker may still be running
    CHECK_FALSE(s.send("too soon"));
    CHECK(s.accept().error().code == "agent.busy");
    CHECK(s.reject().error().code == "agent.busy");
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK_FALSE(s.busy());
    CHECK(error.find("cancel") != std::string::npos);
    CHECK(started == 0);  // the queued call never ran
    CHECK(rig.tracks("Cancelled") == 0);
    // The history still answers every call, so the next request is well formed.
    const auto history = s.history();
    REQUIRE(history.size() == 3);
    REQUIRE(history[2].toolResults.size() == 1);
    CHECK(history[2].toolResults[0].isError);
    CHECK(history[2].toolResults[0].content == "cancelled");

    idle = false;
    error.clear();
    REQUIRE(s.send("Go on."));
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK(error.empty());
    const auto reqs = server.requests();
    REQUIRE(reqs.size() == 2);
    const json::Value& result = el(at(el(at(reqs[1], "messages"), 2), "content"), 0);
    CHECK(at(result, "type").asString() == "tool_result");
    CHECK(at(result, "is_error").asBool());
    REQUIRE(s.reject());
}

TEST_CASE("the agent cannot switch branches, open projects or save elsewhere") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn(
        "", {
                {"exec", R"({"command": "branch.switch", "args": {"name": "main"}})"},
                {"exec", R"({"command": "project.new@1", "args": {}})"},
                {"exec", R"({"command": "engine.start", "args": {}})"},
                {"exec", R"({"batch": {"steps": [{"command": "track.add", "args": {"name": "Ok"}},
                    {"command": "branch.delete", "args": {"name": "main"}}]}})"},
                {"exec", R"({"command": "batch", "args": {"steps": [
                    {"command": "branch.merge", "args": {"from": "main"}}]}})"},
                {"batch", R"({"steps": [{"command": "branch.create", "args": {"name": "x"}}]})"},
                {"branch_switch", R"({"name": "main"})"},
                {"project_open", R"({"path": "other.json"})"},
                {"project_save", R"({"path": "elsewhere.json"})"},
                {"exec", R"({"command": "track.add", "args": {"name": "Allowed"}})"},
            }));
    server.push(anthropicTurn("Done.", {}));
    Session s({Endpoint::Kind::Anthropic, "fake", server.url(), "m", "k"}, rig.registry,
              *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    bool idle = false;
    std::vector<ToolResult> results;
    s.events.idle = [&] { idle = true; };
    s.events.toolFinished = [&](const ToolCall&, const ToolResult& r) { results.push_back(r); };
    s.events.error = [&](const std::string& e) { FAIL(e); };
    REQUIRE(s.send("Try everything."));
    REQUIRE(rig.pump([&] { return idle; }));
    REQUIRE(results.size() == 10);
    for (std::size_t i = 0; i + 1 < results.size(); ++i) {
        INFO(i << ": " << results[i].content);
        CHECK(results[i].isError);
        CHECK(results[i].content.find("agent.denied") != std::string::npos);
    }
    CHECK_FALSE(results.back().isError);
    CHECK(server.requests().size() == 2);  // denials do not stop the turn
    CHECK(rig.branch() == "agent/panel/1");
    CHECK(rig.tracks("Allowed") == 1);
    CHECK(rig.tracks("Ok") == 0);  // the batch was refused as a whole
    auto list = rig.dispatcher->execute({"branch.list", json::Value::object(), {}});
    REQUIRE(list);
    CHECK(json::writeCompact(*list).find("\"main\"") != std::string::npos);
    CHECK(json::writeCompact(*list).find("\"x\"") == std::string::npos);
    REQUIRE(s.reject());
}

TEST_CASE("a branch switched under the agent fails its next tool call and stops the turn") {
    Rig rig;
    FakeServer server;
    server.push(
        anthropicTurn("", {{"exec", R"({"command": "track.add", "args": {"name": "Late"}})"},
                           {"exec", R"({"command": "track.add", "args": {"name": "Later"}})"}}));
    server.push(anthropicTurn("Never requested.", {}));
    Session s({Endpoint::Kind::Anthropic, "fake", server.url(), "m", "k"}, rig.registry,
              *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    bool idle = false;
    std::string error;
    std::vector<ToolResult> results;
    s.events.idle = [&] { idle = true; };
    s.events.error = [&](const std::string& e) { error = e; };
    s.events.toolFinished = [&](const ToolCall&, const ToolResult& r) { results.push_back(r); };
    REQUIRE(s.send("Add a track."));
    REQUIRE(rig.waitPosted(2));
    // The human switches back to their version before the call runs.
    json::Value sw = json::Value::object();
    sw["name"] = "main";
    REQUIRE(rig.dispatcher->execute({"branch.switch", sw, {}}));
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK(error.find("agent.branch_changed") != std::string::npos);
    REQUIRE(results.size() == 1);  // the second call never ran
    CHECK(results[0].isError);
    CHECK(results[0].content.find("agent.branch_changed") != std::string::npos);
    CHECK(rig.branch() == "main");
    CHECK(rig.tracks("Late") == 0);
    CHECK(server.requests().size() == 1);
    const auto history = s.history();
    REQUIRE(history.back().toolResults.size() == 2);
    CHECK(history.back().toolResults[1].content == "not run: the turn stopped");
}

TEST_CASE("nested Lua cannot switch out of the agent branch") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn(
        "",
        {{"exec",
          R"({"command": "script.run", "args": {"code": "q.branch.switch{name = 'main'}"}})"}}));
    server.push(anthropicTurn("Never requested.", {}));
    Session s({Endpoint::Kind::Anthropic, "fake", server.url(), "m", "k"}, rig.registry,
              *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    bool idle = false;
    std::string error;
    s.events.idle = [&] { idle = true; };
    s.events.error = [&](const std::string& e) { error = e; };
    REQUIRE(s.send("Run a script."));
    REQUIRE(rig.pump([&] { return idle; }));
    CHECK(error.empty());
    CHECK(rig.app->versions().branch() == s.branch());
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK(json::writeCompact(requests[1]).find("agent.denied") != std::string::npos);
}

// Diagnostic test asserting current buggy behavior, not a proposed regression.
TEST_CASE("audit3: review refuses namesake branches in a replacement project") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn("Old proposal", {}));
    Session session({Endpoint::Kind::Anthropic, "fake", server.url(), "model", ""}, rig.registry,
                    *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    REQUIRE(session.send("Start"));
    REQUIRE(rig.pump([&] { return !session.busy(); }));
    const auto name = session.branch();
    REQUIRE(rig.app->newProject({}));
    json::Value create = json::Value::object();
    create["name"] = name;
    create["switch"] = true;
    REQUIRE(rig.dispatcher->execute({"branch.create", create, {}}));
    json::Value add = json::Value::object();
    add["name"] = "UNRELATED PROPOSAL";
    REQUIRE(rig.dispatcher->execute({"track.add", add, {}}));
    auto diff = session.diff();
    CHECK_FALSE(diff);
    if (!diff) CHECK(diff.error().code == "agent.project_changed");
    CHECK_FALSE(session.audition(false));
    CHECK_FALSE(session.audition(true));
    auto accepted = session.accept();
    CHECK_FALSE(accepted);
    if (!accepted) CHECK(accepted.error().code == "agent.project_changed");
    CHECK_FALSE(session.reject());
    CHECK_FALSE(session.send("Follow up"));
    CHECK(rig.branch() == name);
    CHECK(rig.tracks("UNRELATED PROPOSAL") == 1);
    CHECK(rig.app->versions().head(name));
}

TEST_CASE("audit3: proposal review refuses a changed project path") {
    Rig rig;
    FakeServer server;
    server.push(anthropicTurn("Proposal", {}));
    Session session({Endpoint::Kind::Anthropic, "fake", server.url(), "model", ""}, rig.registry,
                    *rig.dispatcher, rig.post(), skillOptions(rig.dir / "skill"));
    REQUIRE(session.send("Start"));
    REQUIRE(rig.pump([&] { return !session.busy(); }));
    const auto generation = rig.app->projectGeneration();
    REQUIRE(rig.app->save(rig.dir / "renamed.json"));
    REQUIRE(rig.app->projectGeneration() == generation);
    std::string message;
    session.events.error = [&](const auto& error) { message = error; };
    CHECK_FALSE(session.diff());
    CHECK_FALSE(session.audition(false));
    CHECK_FALSE(session.accept());
    CHECK_FALSE(session.reject());
    CHECK_FALSE(session.send("Follow up"));
    CHECK(message.find("different project") != std::string::npos);
}
