#include "supple/agent/session.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <set>
#include <string_view>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/mcp/server.h"
#include "supple/product.h"

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

json::Value errorJson(const Error& error) {
    json::Value v = json::Value::object();
    v["code"] = error.code;
    v["message"] = error.message;
    v["path"] = error.path;
    v["hint"] = error.hint;
    return v;
}

// Commands that change which project or branch the agent's edits land on, or
// the app's lifecycle: the human's decisions (the panel's A/B, accept and
// reject drive the branches).
constexpr std::string_view kDenied[] = {"branch.create", "branch.delete", "branch.merge",
                                        "branch.switch", "engine.start",  "engine.stop",
                                        "project.new",   "project.open"};

Status admitCommand(std::string_view command, const json::Value& args, const std::string& path) {
    command = command.substr(0, command.find('@'));  // "name@version"
    const bool saveAs = command == "project.save" && !at(args, "path").isNull();
    if (!saveAs && std::find(std::begin(kDenied), std::end(kDenied), command) == std::end(kDenied))
        return {};
    return Error("agent.denied",
                 std::string(command) + (saveAs ? " with a path" : "") +
                     " is not available to the agent: the human manages branches, projects and "
                     "the engine",
                 path,
                 "keep editing on your branch; the human A/Bs it, then accepts or rejects it");
}

// A batch's steps (malformed batches are left to the dispatcher).
Status admitBatch(const json::Value& batch, const std::string& path) {
    const json::Value& steps = at(batch, "steps");
    if (!steps.isArray()) return {};
    for (std::size_t i = 0; i < steps.asArray().size(); ++i) {
        const json::Value& step = steps.asArray()[i];
        if (!at(step, "command").isString()) continue;
        if (auto ok = admitCommand(at(step, "command").asString(), at(step, "args"),
                                   path + "/steps/" + std::to_string(i) + "/command");
            !ok)
            return ok;
    }
    return {};
}

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
    // The MCP server's dedicated tools: one per mcp-tool command, dots as underscores.
    for (const cmd::CommandInfo* info : registry.all())
        if (registry.find(info->name) == info && info->exposure == cmd::Exposure::McpTool) {
            std::string tool = info->name;
            std::replace(tool.begin(), tool.end(), '.', '_');
            toolCommands_[tool] = info->name;
        }
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
                      "Read a topic of the " + std::string(product::kName) +
                          " skill (guides and generated references).",
                      skill});
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
    if (!branch_.empty()) return audition(true);
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
    auto project = exec("project.get", json::Value::object());
    if (!project) {
        if (events.error) events.error("could not read the project: " + project.error().message);
        return false;
    }
    projectGeneration_ =
        at(*project, "generation").isInt() ? at(*project, "generation").asInt() : 0;
    projectPath_ = at(*project, "path").isString() ? at(*project, "path").asString() : "";
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
    worker_ = std::jthread([this, alive = std::weak_ptr<int>(alive_), generation = ++generation_](
                               std::stop_token stop) { work(stop, alive, generation); });
    return true;
}

void Session::cancel() {
    if (!busy_) return;
    ++generation_;  // what the worker already posted for this turn becomes a no-op
    worker_.request_stop();
}

Status Session::admit(const ToolCall& c) const {
    if (c.name == "exec") {
        if (const json::Value& batch = at(c.input, "batch"); !batch.isNull())
            return admitBatch(batch, "/arguments/batch");
        if (!at(c.input, "command").isString()) return {};
        const std::string& command = at(c.input, "command").asString();
        if (command.substr(0, command.find('@')) == "batch")
            return admitBatch(at(c.input, "args"), "/arguments/args");
        return admitCommand(command, at(c.input, "args"), "/arguments/command");
    }
    auto it = toolCommands_.find(c.name);
    if (it == toolCommands_.end()) return {};  // describe, query, skill_read, listen
    if (it->second == "batch") return admitBatch(c.input, "/arguments");
    return admitCommand(it->second, c.input, "/name");
}

Status Session::pinned() {
    auto project = exec("project.get", json::Value::object());
    const std::string hint = "switch back to \"" + branch_ +
                             "\" (the panel's A/B) and send again, or reject the agent's branch";
    if (!project)
        return Error("agent.branch_changed",
                     "cannot read the current branch: " + project.error().message, {}, hint);
    const json::Value &current = at(*project, "branch"), &path = at(*project, "path");
    if (!current.isString() || current.asString() != branch_)
        return Error("agent.branch_changed",
                     "the current branch is \"" +
                         (current.isString() ? current.asString() : std::string()) +
                         "\", not the agent's branch \"" + branch_ + "\"",
                     {}, hint);
    if (at(*project, "generation").asInt() != projectGeneration_ || !path.isString() ||
        path.asString() != projectPath_)
        return Error("agent.branch_changed",
                     "the project file changed from \"" + projectPath_ + "\" to \"" +
                         (path.isString() ? path.asString() : std::string()) + "\"",
                     {}, "send again to continue in this project");
    return {};
}

ToolResult Session::call(const ToolCall& c, std::optional<Error>& stopTurn) {
    ToolResult r{c.id, {}, false};
    auto failed = [&](const Error& error) {
        r.content = json::writeCompact(errorJson(error));
        r.isError = true;
        return r;
    };
    if (auto ok = pinned(); !ok) {
        stopTurn = ok.error();
        return failed(ok.error());
    }
    if (auto ok = admit(c); !ok) return failed(ok.error());
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
        auto guarded = dispatcher_.guarded(
            [&](const cmd::Invocation& invocation) -> Status {
                if (auto allowed = admitCommand(invocation.command, invocation.args, "/command");
                    !allowed)
                    return allowed;
                // pinned() queries this command itself.
                if (invocation.command.substr(0, invocation.command.find('@')) == "project.get")
                    return {};
                auto same = pinned();
                if (!same) stopTurn = same.error();
                return same;
            },
            [&]() -> Result<json::Value> {
                auto response = mcp_->handle(req);
                return response ? std::move(*response) : json::Value();
            });
        std::optional<json::Value> resp;
        if (guarded && !guarded->isNull()) resp = std::move(*guarded);
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
    if (auto ok = pinned(); !ok) {  // a script (or the human) moved it while the call ran
        stopTurn = ok.error();
        return failed(ok.error());
    }
    if (r.content.size() > kMaxToolResult)
        r.content = r.content.substr(0, kMaxToolResult) + "\n… (truncated; ask for less detail)";
    return r;
}

void Session::work(std::stop_token stop, std::weak_ptr<int> alive, std::uint64_t generation) {
    // Events of this turn: dropped once it is cancelled (or the session is gone).
    auto onModel = [this, alive, generation](std::function<void()> f) {
        post_([this, alive, generation, f = std::move(f)] {
            if (alive.lock() && generation_ == generation) f();
        });
    };
    // The worker's last post: only now is the session idle.
    auto finish = [&](std::optional<std::string> error) {
        post_([this, alive, error] {
            if (!alive.lock()) return;
            busy_ = false;
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
        // Run each call on the model thread and wait. Every call gets a result
        // (the next request needs one per call), real or "not run".
        struct Outcome {
            ToolResult result;
            std::optional<Error> stopTurn;
        };
        Message results;
        std::optional<std::string> stopWith;
        for (const ToolCall& c : turn->message.toolCalls) {
            if (stopWith || stop.stop_requested()) {
                results.toolResults.push_back(
                    {c.id, stopWith ? "not run: the turn stopped" : "cancelled", true});
                continue;
            }
            auto promise = std::make_shared<std::promise<Outcome>>();
            auto future = promise->get_future();
            post_([this, alive, generation, c, promise] {
                if (!alive.lock() || generation_ != generation)  // cancelled: never runs
                    return promise->set_value({{c.id, "cancelled", true}, std::nullopt});
                Outcome o;
                if (events.toolStarted) events.toolStarted(c);
                o.result = call(c, o.stopTurn);
                if (events.toolFinished) events.toolFinished(c, o.result);
                promise->set_value(std::move(o));
            });
            while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready &&
                   !stop.stop_requested()) {
            }
            if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                // Cancelled while queued: the call is invalidated and will not run.
                results.toolResults.push_back({c.id, "cancelled", true});
                continue;
            }
            Outcome o = future.get();
            if (o.stopTurn) stopWith = o.stopTurn->toString();
            results.toolResults.push_back(std::move(o.result));
        }
        const bool listened =
            std::any_of(turn->message.toolCalls.begin(), turn->message.toolCalls.end(),
                        [](const ToolCall& c) { return c.name == "listen"; });
        {
            std::lock_guard lock(mutex_);
            history_.push_back(std::move(results));
        }
        if (stopWith) return finish(*stopWith);
        if (stop.stop_requested()) return finish("cancelled");
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
