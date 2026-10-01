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

#include "fake_llm.h"
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
