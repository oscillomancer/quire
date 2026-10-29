// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <memory>

#include "supple/engine/app.h"
#include "supple/model/freeze.h"
#include "supple/model/numeric.h"

namespace supple::engine {
// A locked item refuses every change except being unlocked (item.set
// {locked: false}); its takes and MIDI with it.
inline Status checkUnlocked(const model::Session& s, model::Id item, std::string path = "/item") {
    if (auto owner = model::frozenOwner(s, item)) return model::frozenError(s, *owner, path);
    if (const model::Item* it = s.items.find(item); it && it->locked)
        return Error("item.locked", item.str() + " is locked", std::move(path),
                     "unlock it with item.set {item: \"" + item.str() + "\", locked: false}");
    return {};
}
inline Status checkUnlockedTake(const model::Session& s, model::Id take, std::string path) {
    for (const auto& e : s.items)
        if (std::find(e.value->takes.begin(), e.value->takes.end(), take) != e.value->takes.end())
            return checkUnlocked(s, e.id, std::move(path));
    return {};
}

// Removes a take with its FX, the envelopes on it and its FX, and its MIDI
// source when nothing else uses it (take.remove, item.remove; take_commands.cpp).
// The item's take list is the caller's to update.
void removeTakeObjects(model::EditTxn& t, const model::Session& s, model::Id take);

// Validate derived edit values before the invocation succeeds, so a caught Lua
// error restores this invocation's savepoint even inside a larger transaction.
template <class Args, class Output>
void addChecked(cmd::Registry& registry, cmd::Command<Args, Output> command) {
    if (command.kind != cmd::Kind::Query) {
        auto original = std::move(command.run);
        auto encoded = command.args;
        const auto name = command.name;
        const bool edit = command.kind == cmd::Kind::Edit;
        command.run = [original = std::move(original), encoded, name, edit](
                          const Args& a, cmd::Context& ctx) -> Result<Output> {
            auto& app = ctx.service<EngineApp>();
            const auto before = app.view();
            const auto fields = encoded.encode(a);
            if (name != "track.unfreeze" && name != "track.freeze.commit" &&
                name != "track.remove" && name != "track.duplicate") {
                for (const auto& field : fields.asObject()) {
                    if (!field.value.isString()) continue;
                    if (field.key != "fx" && field.key != "item" && field.key != "take" &&
                        field.key != "source" && field.key != "env" && field.key != "track")
                        continue;
                    auto id = model::Id::parse(field.value.asString());
                    if (!id) continue;
                    auto owner = model::frozenOwner(before, *id);
                    if (!owner) continue;
                    if (field.key == "track") {
                        if (name == "track.move") continue;
                        if (name == "track.set") {
                            bool sound = false;
                            for (const auto& f : fields.asObject())
                                if (f.key != "track" && f.key != "volume_db" && f.key != "pan" &&
                                    f.key != "mute" && f.key != "solo" && f.key != "name" &&
                                    f.key != "color" && f.key != "notes" &&
                                    f.key != "parent_send" && f.key != "locked" &&
                                    f.key != "hidden_tcp" && f.key != "hidden_mixer")
                                    sound = true;
                            if (!sound) continue;
                        }
                        if (name.starts_with("route.") || name.starts_with("env.")) continue;
                    }
                    return model::frozenError(before, *owner, "/" + field.key);
                }
            }
            auto result = original(a, ctx);
            if (!result) return result.error();
            if (edit) SUPPLE_TRY(model::validateFrozenTransition(before, app.view()));
            return result;
        };
    }
    if (command.kind == cmd::Kind::Edit) {
        auto run = std::move(command.run);
        // Shared, not copied: commands are copied with their closures, and a
        // schema copy is a deep copy of its JSON and field closures.
        auto args = std::make_shared<const decltype(command.args)>(command.args);
        command.run = [run = std::move(run), args](const Args& a,
                                                   cmd::Context& ctx) -> Result<Output> {
            auto result = run(a, ctx);
            if (!result) return result.error();
            auto& app = ctx.service<EngineApp>();
            auto session = app.view();
            if (auto* engine = app.engine();
                engine && engine->config().sampleRate > session.project->sampleRate) {
                auto project = *session.project;
                project.sampleRate = engine->config().sampleRate;
                session.project = std::make_shared<const model::Project>(std::move(project));
            }
            if (auto valid = model::validateNumeric(session); !valid) {
                Error error = valid.error();
                // When an argument directly sets this field, identify that argument.
                const auto slash = error.path.rfind('/');
                const auto field = error.path.substr(slash == std::string::npos ? 0 : slash + 1);
                if (args->encode(a).find(field)) error.path = "/" + field;
                return error;
            }
            return result;
        };
    }
    registry.add(std::move(command));
}
}  // namespace supple::engine
