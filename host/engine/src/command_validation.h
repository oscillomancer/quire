#pragma once

#include <memory>

#include "supple/engine/app.h"
#include "supple/model/numeric.h"

namespace supple::engine {
// Validate derived edit values before the invocation succeeds, so a caught Lua
// error restores this invocation's savepoint even inside a larger transaction.
template <class Args, class Output>
void addChecked(cmd::Registry& registry, cmd::Command<Args, Output> command) {
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
