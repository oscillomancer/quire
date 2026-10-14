// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/lua/runtime.h"

#include <supple/schema/schema.h>

#include <cstdint>
#include <utility>

namespace supple::lua {

namespace {
// A handler that changes state emits further events, which are delivered after
// the command it ran — the same rule one level down. A script that reacts to
// its own event would otherwise never stop, so delivery stops this many levels
// deep and the events past it are dropped and counted.
constexpr int kMaxDeliveryDepth = 8;

struct RunArgs {
    std::string code;
    std::optional<std::string> name;
    std::optional<double> budget_ms;
    std::optional<json::Value> args;
};

struct RunResult {
    json::Value result;
    std::vector<std::string> log;
    std::string script;
    std::int64_t instructions = 0;
};
}  // namespace

Runtime::Runtime(const cmd::Registry* registry, ScriptOptions defaults)
    : registry_(registry), defaults_(std::move(defaults)) {}

Runtime::~Runtime() = default;

void Runtime::setCommands(const cmd::Registry& registry, cmd::Dispatcher& dispatcher) {
    registry_ = &registry;
    dispatcher_ = &dispatcher;
    for (auto& [name, script] : scripts_) script->setCommands(registry_, dispatcher_);
}

Result<ScriptResult> Runtime::run(std::string_view name, std::string_view source, double budgetMs,
                                  const json::Value& args) {
    auto& slot = scripts_[std::string(name)];
    if (!slot) {
        ScriptOptions options = defaults_;
        options.name = std::string(name);
        slot = std::make_unique<Script>(registry_, dispatcher_, std::move(options));
    }
    return slot->run(source, budgetMs, args);
}

Script* Runtime::find(std::string_view name) {
    auto found = scripts_.find(std::string(name));
    return found == scripts_.end() ? nullptr : found->second.get();
}

bool Runtime::active() const {
    if (deliveryDepth_) return true;
    for (const auto& [name, script] : scripts_)
        if (script->running()) return true;
    return false;
}

void Runtime::clear() {
    scripts_.clear();
    pending_.clear();
    droppedEvents_ = 0;
}

std::function<void()> Runtime::eventSavepoint() {
    return [this, saved = pending_]() mutable { pending_ = std::move(saved); };
}

void Runtime::event(std::string_view topic, json::Value payload) {
    if (scripts_.empty()) return;
    pending_.emplace_back(std::string(topic), std::move(payload));
}

void Runtime::deliver() {
    if (pending_.empty()) return;
    if (deliveryDepth_ >= kMaxDeliveryDepth) {
        droppedEvents_ += pending_.size();
        pending_.clear();
        return;
    }
    auto events = std::exchange(pending_, {});
    ++deliveryDepth_;
    for (const auto& [topic, payload] : events)
        for (auto& [name, script] : scripts_) {
            (void)name;
            // A handler may run commands: their events queue here and are
            // delivered by the commandFinished() of the command they ran.
            script->publish(topic, payload);
        }
    --deliveryDepth_;
}

// -----------------------------------------------------------------------------
// script.run
// -----------------------------------------------------------------------------

void registerScriptCommands(cmd::Registry& registry) {
    using namespace schema;

    auto args =
        object<RunArgs>("script_run_args")
            .doc(
                "Lua 5.4 run on the model thread in a sandbox: string, table, math, "
                "utf8 and coroutine only, no io, os, debug or package. `q` holds one "
                "function per registered command (q.track.add{name = \"Bass\"} returns "
                "the result as a table and raises `{code, message, path, hint, details}` "
                "on failure), `q.batch(label, function() ... end)` runs one transaction, "
                "`q.on(topic, function(payload, topic) ... end)` subscribes to bus events "
                "and `q.run(name, args)` calls a command the bindings cannot name. The "
                "script's commands run as the origin `lua:<name>` and are recorded in the "
                "undo history like any other edit.")
            .field("code", &RunArgs::code,
                   string()
                       .doc("The Lua source. It may return a value: what it returns is "
                            "converted to JSON (tables with string keys are objects, "
                            "sequences are arrays).")
                       .minLength(1))
            .field("name", &RunArgs::name,
                   string()
                       .doc("The script's name: its commands are recorded as "
                            "`lua:<name>` and its state (globals, subscriptions) is "
                            "kept between runs of the same name. Defaults to the "
                            "caller's client name.")
                       .minLength(1)
                       .maxLength(64))
            .field("budget_ms", &RunArgs::budget_ms,
                   number().range(1, 10000).doc("Instruction budget of this run; the script "
                                                "is stopped with `script.budget` when it "
                                                "runs out. Time spent inside a command does "
                                                "not count."))
            .field("args", &RunArgs::args, any().doc("Converted to the global table `args`."))
            .build();
    auto result =
        object<RunResult>("script_run_result")
            .field("result", &RunResult::result,
                   any().doc("What the script returned (null if it returned nothing)."))
            .field("log", &RunResult::log, arrayOf(string()).doc("What the script printed."))
            .field("script", &RunResult::script, string().doc("The name it ran under."))
            .field("instructions", &RunResult::instructions,
                   integer().doc("Lua instructions the run executed."))
            .build();

    cmd::Command<RunArgs, RunResult> command;
    command.name = "script.run";
    command.kind = cmd::Kind::Action;
    command.exposure = cmd::Exposure::McpExec;
    command.summary = "Run a Lua script on the command bus.";
    command.description =
        "Every call the script makes goes through the command registry, so it is validated, "
        "transactional and in the undo history like a call from the GUI or an agent. Use it for "
        "loops, conditionals and arithmetic that the command set cannot express; use batch for a "
        "fixed list of steps. Not transactional itself: undo the edits it made with history.undo.";
    command.tags = {"script"};
    command.args = args;
    command.result = result;
    command.errors = {
        {"script.syntax", "the source does not compile", "fix the Lua syntax at the reported line"},
        {"script.budget", "the run spent its instruction budget",
         "raise budget_ms or make the script do less between waits"},
        {"script.memory", "the run hit the script's memory cap",
         "raise memory_limit_bytes or hold less data"},
        {"script.error", "the script raised a Lua error", "read the message and the traceback"},
        {"script.arguments", "a command was called with something other than a table",
         "call it with a table of arguments, e.g. q.track.add{name = \"Bass\"}"},
        {"script.unavailable", "no command dispatcher is attached to the script",
         "run the script through a host that provides a Runtime"},
    };
    // The example is built in code: a JSON literal with an embedded Lua chunk
    // is a quoting trap.
    json::Value exampleArgs = json::Value::object();
    exampleArgs["code"] =
        "local total = 0\n"
        "for i = 1, 4 do total = total + i end\n"
        "return {total = total, steps = {1, 2, 3, 4}}";
    json::Value exampleResult = json::Value::object();
    json::Value returned = json::Value::object();
    returned["total"] = 10;
    returned["steps"] = json::Value::array();
    for (int i = 1; i <= 4; ++i) returned["steps"].push_back(i);
    exampleResult["result"] = std::move(returned);
    exampleResult["log"] = json::Value::array();
    exampleResult["script"] = "<any>";
    exampleResult["instructions"] = "<any>";
    command.examples = {{"A script that computes, with no commands", std::move(exampleArgs),
                         std::move(exampleResult), "empty"}};
    command.run = [](const RunArgs& arguments, cmd::Context& context) -> Result<RunResult> {
        if (!context.has<Runtime>())
            return Error("script.unavailable", "this host has no script runtime", {},
                         "the application must provide a lua::Runtime in its context");
        std::string name = arguments.name.value_or(context.origin().client);
        if (name.empty()) name = "script";
        auto outcome =
            context.service<Runtime>().run(name, arguments.code, arguments.budget_ms.value_or(-1.0),
                                           arguments.args.value_or(json::Value()));
        if (!outcome) return outcome.error();
        RunResult out;
        out.result = outcome->result;
        out.log = outcome->log;
        out.script = outcome->script;
        out.instructions = static_cast<std::int64_t>(outcome->instructions);
        return out;
    };
    registry.add(std::move(command));
}

}  // namespace supple::lua
