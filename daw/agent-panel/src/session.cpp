#include "supple/agent/session.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <set>
#include "supple/json/parse.h"

#include "supple/base/files.h"
#include "supple/json/write.h"
#include "supple/mcp/server.h"

namespace supple::agent {
namespace {

constexpr std::size_t kMaxToolResult = 24000;  // characters the model reads per call

std::string readText(const std::filesystem::path& p) {
    auto bytes = readFile(p);
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string();
}

std::string stripFrontMatter(std::string s) {
    if (s.rfind("---", 0) == 0)
        if (auto end = s.find("\n---", 3); end != std::string::npos)
            s.erase(0, s.find('\n', end + 4) + 1);
    return s;
}

std::vector<std::string> skillTopics(const std::filesystem::path& dir) {
    std::vector<std::string> topics;
    std::error_code ec;
    for (const char* sub : {"guides", "generated"})
        for (const auto& f : std::filesystem::directory_iterator(dir / sub, ec))
            if (f.path().extension() == ".md")
                topics.push_back(std::string(sub) + "/" + f.path().filename().string());
    std::sort(topics.begin(), topics.end());
    return topics;
}

json::Value schemaOf(const char* text) { return *json::parse(text); }

constexpr const char* kPanelRules = R"(

## Working in the DAW's agent panel

- You are inside the human's DAW session, on your own branch (named in the
  first message). Every change you make lands on that branch; the human sees
  a diff, A/B-plays it against their version, and accepts or rejects it.
  Never switch branches yourself.
- Group related edits into one `batch` where possible: each command is one
  step in their undo history.
- When the result depends on taste, call the `listen` tool with what to
  listen for and the region, then stop and wait for the human's reply.
- End with a short summary of what you changed and why, in plain words.
- Load guides on demand with `skill_read` (topics are listed in its schema).
)";

}  // namespace

Session::Session(Endpoint endpoint, const cmd::Registry& registry, cmd::Dispatcher& dispatcher,
                 Post post, Options options)
    : endpoint_(std::move(endpoint)),
      dispatcher_(dispatcher),
      post_(std::move(post)),
      options_(std::move(options)),
      mcp_(std::make_unique<mcp::Server>(registry, dispatcher)) {
    mcp_->setOrigin({cmd::Origin::Type::Agent, options_.client, endpoint_.model});
    auto rpc = [this](const char* method, json::Value params) {
        json::Value req = json::Value::object();
        req["jsonrpc"] = "2.0";
        req["id"] = nextId_++;
        req["method"] = method;
        req["params"] = std::move(params);
        return mcp_->handle(req);
    };
    json::Value hello = schemaOf(R"({"protocolVersion": "2025-06-18", "clientInfo": {}})");
    hello["clientInfo"]["name"] = options_.client;  // recorded as the origin's client
    rpc("initialize", std::move(hello));
    if (auto listed = rpc("tools/list", json::Value::object()); listed && listed->find("result"))
        for (const json::Value& t : at(at((*listed), "result"), "tools").asArray())
            tools_.push_back(
                {at(t, "name").asString(),
                 t.find("description") ? at(t, "description").asString() : "",
                 t.find("inputSchema") ? at(t, "inputSchema") : json::Value::object()});
    // Agent-only tools.
    json::Value topics = json::Value::array();
    for (const std::string& t : skillTopics(options_.skillDir)) topics.push_back(t);
    json::Value skill = schemaOf(
        R"({"type": "object", "properties": {"topic": {"type": "string"}}, "required": ["topic"]})");
    skill["properties"]["topic"]["enum"] = std::move(topics);
    tools_.push_back({"skill_read",
                      "Read a topic of the Quire skill (guides and generated references).", skill});
    tools_.push_back({"listen",
                      "Ask the human to listen before you decide: what to listen for, where, and "
                      "whether to A/B against their "
                      "version. Then stop and wait for their reply.",
                      schemaOf(R"({"type": "object", "properties": {
            "what": {"type": "string", "description": "what to listen for"},
            "start_bar": {"type": "integer", "minimum": 1}, "end_bar": {"type": "integer", "minimum": 1},
            "ab": {"type": "boolean", "description": "compare against the human's version"}},
            "required": ["what"]})")});
    system_ = stripFrontMatter(readText(options_.skillDir / "SKILL.md")) + kPanelRules;
}

Session::~Session() {
    alive_.reset();
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}

std::vector<Message> Session::history() const {
    std::lock_guard lock(mutex_);
    return history_;
}

Result<json::Value> Session::exec(const std::string& command, json::Value args) {
    return dispatcher_.execute(
        {command, std::move(args), {cmd::Origin::Type::Ui, "gui", "agent-panel"}});
}

Status Session::ensureBranch() {
    if (!branch_.empty()) return {};
    auto list = exec("branch.list", json::Value::object());
    if (!list) return std::move(list).error();
    base_ = at((*list), "current").asString();
    std::set<std::string> names;
    for (const json::Value& b : at((*list), "branches").asArray())
        names.insert(b.isString() ? b.asString() : at(b, "name").asString());
    std::string name;
    for (int i = 1;; ++i) {
        name = "agent/" + options_.client + "/" + std::to_string(i);
        if (!names.count(name)) break;
    }
    json::Value args = json::Value::object();
    args["name"] = name;
    args["switch"] = true;
    auto created = exec("branch.create", std::move(args));
    if (!created) return std::move(created).error();
    branch_ = name;
    return {};
}

bool Session::send(std::string prompt) {
    if (busy_) return false;
    if (auto b = ensureBranch(); !b) {
        if (events.error) events.error("could not start a branch: " + b.error().message);
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        Message m;
        m.text = history_.empty()
                     ? "You are on branch \"" + branch_ + "\" (the human's version is \"" + base_ +
                           "\").\n\n" + prompt
                     : std::move(prompt);
        history_.push_back(std::move(m));
    }
    busy_ = true;
    worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
    return true;
}

void Session::cancel() { worker_.request_stop(); }

ToolResult Session::call(const ToolCall& c) {
    ToolResult r{c.id, {}, false};
    if (c.name == "skill_read") {
        const std::string topic =
            at(c.input, "topic").isString() ? at(c.input, "topic").asString() : "";
        const auto topics = skillTopics(options_.skillDir);
        if (std::find(topics.begin(), topics.end(), topic) == topics.end()) {
            r.content = "unknown topic; use one of the schema's enum values";
            r.isError = true;
        } else {
            r.content = readText(options_.skillDir / topic);
        }
    } else if (c.name == "listen") {
        if (events.listen) events.listen(c.input);
        r.content = "The human has been asked to listen. Stop here and wait for their reply.";
    } else {
        json::Value req = json::Value::object();
        req["jsonrpc"] = "2.0";
        req["id"] = nextId_++;
        req["method"] = "tools/call";
        json::Value params = json::Value::object();
        params["name"] = c.name;
        params["arguments"] = c.input;
        req["params"] = std::move(params);
        auto resp = mcp_->handle(req);
        if (!resp) {
            r.content = "no response";
            r.isError = true;
        } else if (const json::Value* err = resp->find("error")) {
            r.content = json::writeCompact(*err);
            r.isError = true;
        } else {
            const json::Value& result = at((*resp), "result");
            for (const json::Value& part : at(result, "content").asArray())
                if (part.find("text")) r.content += at(part, "text").asString();
            r.isError = at(result, "isError").isBool() && at(result, "isError").asBool();
        }
    }
    if (r.content.size() > kMaxToolResult)
        r.content = r.content.substr(0, kMaxToolResult) + "\n… (truncated; ask for less detail)";
    return r;
}

void Session::work(std::stop_token stop) {
    std::weak_ptr<int> alive = alive_;
    auto onModel = [this, alive](std::function<void()> f) {
        post_([alive, f = std::move(f)] {
            if (alive.lock()) f();
        });
    };
    auto finish = [&](std::optional<std::string> error) {
        busy_ = false;
        onModel([this, error] {
            if (error && events.error) events.error(*error);
            if (events.idle) events.idle();
        });
    };
    for (int round = 0;; ++round) {
        if (stop.stop_requested()) return finish("cancelled");
        if (round >= options_.maxToolRounds)
            return finish("stopped after " + std::to_string(options_.maxToolRounds) +
                          " tool rounds");
        Request req{system_, history(), tools_};
        auto turn = complete(
            endpoint_, req,
            [&](std::string_view delta) {
                onModel([this, d = std::string(delta)] {
                    if (events.text) events.text(d);
                });
            },
            stop);
        if (!turn) return finish(turn.error().message);
        {
            std::lock_guard lock(mutex_);
            history_.push_back(turn->message);
        }
        onModel([this, t = *turn] {
            if (events.turn) events.turn(t);
        });
        if (turn->message.toolCalls.empty()) return finish(std::nullopt);
        // Run each call on the model thread and wait (abandoning on cancel).
        Message results;
        for (const ToolCall& c : turn->message.toolCalls) {
            auto promise = std::make_shared<std::promise<ToolResult>>();
            auto future = promise->get_future();
            onModel([this, c, promise] {
                if (events.toolStarted) events.toolStarted(c);
                ToolResult r = call(c);
                if (events.toolFinished) events.toolFinished(c, r);
                promise->set_value(std::move(r));
            });
            while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
                if (stop.stop_requested()) return finish("cancelled");
            results.toolResults.push_back(future.get());
        }
        const bool listened =
            std::any_of(turn->message.toolCalls.begin(), turn->message.toolCalls.end(),
                        [](const ToolCall& c) { return c.name == "listen"; });
        {
            std::lock_guard lock(mutex_);
            history_.push_back(std::move(results));
        }
        if (listened) return finish(std::nullopt);  // the human answers in the next message
    }
}

Result<json::Value> Session::diff() {
    if (branch_.empty()) return json::Value::object();
    json::Value args = json::Value::object();
    args["a"] = base_;
    args["b"] = branch_;
    return exec("branch.diff", std::move(args));
}

Status Session::audition(bool agentBranch) {
    if (busy_) return Error("agent.busy", "wait until the agent is done");
    if (branch_.empty()) return Error("agent.branch", "the agent has not made changes yet");
    json::Value args = json::Value::object();
    args["name"] = agentBranch ? branch_ : base_;
    auto r = exec("branch.switch", std::move(args));
    if (!r) return std::move(r).error();
    return {};
}

Result<json::Value> Session::accept() {
    if (busy_) return Error("agent.busy", "wait until the agent is done");
    if (branch_.empty()) return Error("agent.branch", "nothing to accept");
    json::Value merge = json::Value::object();
    merge["from"] = branch_;
    merge["into"] = base_;
    auto merged = exec("branch.merge", std::move(merge));
    if (!merged) return merged;
    if (auto r = reject(); !r) return std::move(r).error();  // back to the base, branch gone
    return merged;
}

Status Session::reject() {
    if (busy_) return Error("agent.busy", "wait until the agent is done");
    if (branch_.empty()) return {};
    json::Value sw = json::Value::object();
    sw["name"] = base_;
    if (auto r = exec("branch.switch", std::move(sw)); !r) return std::move(r).error();
    json::Value del = json::Value::object();
    del["name"] = branch_;
    if (auto r = exec("branch.delete", std::move(del)); !r) return std::move(r).error();
    branch_.clear();
    return {};
}

}  // namespace supple::agent
