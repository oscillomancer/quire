// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The scripts of one application and the bus events that reach them
// (ADR-0006). Model thread only.
//
// A Runtime keeps one persistent Lua state per script name, so a script can
// subscribe with `q.on` and keep reacting to later changes instead of polling.
// The host queues the events its bus emits (`event`) and hands them to the
// subscriptions when the emitting command has finished (`deliver`).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/json/json.h"
#include "supple/lua/script.h"

namespace supple::lua {

class Runtime {
  public:
    // `registry` generates the bindings, `defaults` are the options of every
    // script it makes. A host that does not have the registry yet passes null
    // and calls setCommands() once it does (scripts made in between are
    // rebound: their bindings are added, never removed).
    explicit Runtime(const cmd::Registry* registry = nullptr, ScriptOptions defaults = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // The commands the scripts may call, and where those calls go. Call it
    // before the first run if you can; scripts made earlier are rebound.
    void setCommands(const cmd::Registry& registry, cmd::Dispatcher& dispatcher);
    bool ready() const { return registry_ != nullptr && dispatcher_ != nullptr; }

    // Runs `source` as the script `name` (creating its state on first use).
    Result<ScriptResult> run(std::string_view name, std::string_view source, double budgetMs,
                             const json::Value& args = json::Value());
    Script* find(std::string_view name);
    std::size_t size() const { return scripts_.size(); }
    bool active() const;  // a script or event delivery is on the stack
    void clear();         // forget every script (a new project)

    // Events of the bus, in emission order, delivered by deliver().
    void event(std::string_view topic, json::Value payload);
    // Restore queued events after a failed invocation; invoke at most once.
    std::function<void()> eventSavepoint();
    void deliver();
    std::size_t pendingEvents() const { return pending_.size(); }
    // Events dropped because handlers kept emitting them (see kMaxDeliveryDepth).
    std::size_t droppedEvents() const { return droppedEvents_; }

  private:
    const cmd::Registry* registry_ = nullptr;
    ScriptOptions defaults_;
    cmd::Dispatcher* dispatcher_ = nullptr;
    std::map<std::string, std::unique_ptr<Script>> scripts_;
    std::vector<std::pair<std::string, json::Value>> pending_;
    int deliveryDepth_ = 0;
    std::size_t droppedEvents_ = 0;
};

// Registers `script.run` (the engine and the sandbox): runs Lua source on the
// model thread with the script's commands recorded under `lua:<name>`. The
// handler takes the host's Runtime from its Context, so a host that registers
// this command must provide one in makeContext.
void registerScriptCommands(cmd::Registry& registry);

}  // namespace supple::lua
