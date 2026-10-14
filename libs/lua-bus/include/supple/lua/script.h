// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A Lua script running against the command bus (ADR-0006). Model thread only:
// a script never touches the audio thread, and every call it makes goes
// through the same registry, validation and transactions as the GUI or an
// agent, so it appears in the undo history like any other edit.
//
// The binding surface is generated from the command registry at runtime
// (`q.track.add{name = "Bass"}`), so it cannot drift from the bus. Errors are
// raised as tables `{code, message, path, hint, details}` that `pcall` can
// catch and re-raise unchanged.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/json/json.h"

struct lua_State;

namespace supple::lua {

struct ScriptOptions {
    // Recorded as the origin of every command the script runs: `lua:<name>`.
    std::string name;
    // Allocator cap for this script's state, in bytes. The cap is a hard
    // ceiling: an allocation past it fails the call with `script.memory`.
    std::uint64_t memoryLimitBytes = 8u << 20;
    // Default instruction budget of one run (`script.run`'s `budget_ms`).
    double defaultBudgetMs = 250.0;
    // Instructions between budget checks. 1 counts (and checks) every
    // instruction, which makes ScriptResult::instructions exact at some cost
    // in pure-Lua speed; raise it for scripts that do long Lua loops.
    std::uint64_t hookCount = 1;
    // Instruction cap of one run on top of the time budget (0: none). It is
    // deterministic where the time budget is not; either one spent ends the
    // run with `script.budget`, and pcall cannot resume after it.
    std::uint64_t instructionLimit = 0;
    // Log lines kept per script (the oldest are dropped).
    std::size_t logLimitLines = 1000;
};

struct ScriptResult {
    json::Value result;              // the script's return value (null if none)
    std::vector<std::string> log;    // what this run printed
    std::string script;              // the name the run was recorded under
    std::uint64_t instructions = 0;  // Lua instructions the run executed (exact at hookCount 1)
};

struct ScriptImpl;

class Script {
  public:
    // `registry` generates the bindings and `dispatcher` takes the calls; a
    // host attaches both before the first run (Runtime::setCommands rebinds
    // the scripts it already made). Null is allowed for a script that only
    // computes: it then has just `q.run`/`q.batch`/`q.on`/`q.off`/`q.log`, and
    // any command call fails with `script.unavailable`.
    Script(const cmd::Registry* registry, cmd::Dispatcher* dispatcher, ScriptOptions options = {});
    ~Script();
    Script(const Script&) = delete;
    Script& operator=(const Script&) = delete;

    const ScriptOptions& options() const;
    bool running() const;
    // The origin every call the script runs carries (Lua, `lua:<name>`).
    cmd::Origin origin() const;

    // Compiles and runs `source`; `args` becomes the global table `args`.
    // The state survives between runs, so globals and subscriptions persist.
    // `budgetMs` < 0 uses the option default.
    //
    // Errors: `script.syntax` (the chunk did not compile) and `script.error`
    // (a Lua error that is not a command error); `script.memory` when the
    // allocator cap is hit, `script.budget` when the instruction budget runs
    // out, and otherwise the command's own error (raised in the script as a
    // table) unchanged.
    Result<ScriptResult> run(std::string_view source, double budgetMs = -1.0,
                             const json::Value& args = json::Value());

    // Hands one bus event to the script's subscriptions. The host calls this
    // after the command that emitted the event, on the model thread.
    void publish(std::string_view topic, const json::Value& payload);

    // Every line the script has printed, oldest first (the most recent
    // logLimitLines of them).
    std::vector<std::string> log() const;
    // Subscriptions currently live (tests, diagnostics).
    std::size_t subscriptionCount() const;

  private:
    friend class Runtime;  // setCommands(): rebind scripts made before a host attached one
    void setCommands(const cmd::Registry* registry, cmd::Dispatcher* dispatcher);
    std::unique_ptr<ScriptImpl> impl_;
};

}  // namespace supple::lua
