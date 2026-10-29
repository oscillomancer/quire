// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstdio>
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

namespace supple::gui {
// UI continuations/status for detection/quantize and background consolidation.
class AudioEdit {
  public:
    AudioEdit(engine::EngineApp& app, cmd::Dispatcher& dispatcher)
        : app_(app), dispatcher_(dispatcher) {}
    void run(const std::vector<model::Id>& selected, const std::string& operation, double grid) {
        if (pending_ || consolidation_) {
            report("An audio edit is already running");
            return;
        }
        const auto s = app_.head();
        auto args = json::Value::object(), detect = json::Value::object();
        args["items"] = json::Value::array();
        detect["takes"] = json::Value::array();
        bool missing = false;
        for (auto id : selected) {
            const auto* it = s.items.find(id);
            const auto* t = it && it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
            const auto* md = t && t->source ? s.media.find(*t->source) : nullptr;
            if (!md) continue;
            args["items"].push_back(id.str());
            detect["takes"].push_back(it->activeTake->str());
            missing = missing || !t->transients || t->transients->sourceHash != md->hash;
        }
        if (!args["items"].size()) {
            report("Select audio items first");
            return;
        }
        args["grid"] = grid;
        args["mode"] = operation;
        args["strength"] = operation == "slice" ? 0. : 1.;
        if (operation == "reset") {
            args.asObject().erase("grid");
            args.asObject().erase("mode");
            args.asObject().erase("strength");
            execute("audio.warp.reset", args);
            return;
        }
        if (operation == "detect" || missing) {
            auto job = dispatcher_.execute({"take.hits.detect", detect, origin_});
            if (!job) {
                report(job.error().toString());
                return;
            }
            pending_ =
                Pending{job->find("job")->asString(), operation == "detect" ? json::Value() : args,
                        s, app_.projectGeneration(), app_.versions().branch()};
            report("Detecting audio hits…");
        } else
            execute("audio.quantize", args);
    }
    void consolidate(const std::vector<model::Id>& selected) {
        if (pending_ || consolidation_) {
            report("An audio edit is already running");
            return;
        }
        auto args = json::Value::object();
        args["items"] = json::Value::array();
        for (auto id : selected) args["items"].push_back(id.str());
        auto job = dispatcher_.execute({"item.consolidate", args, origin_});
        if (!job) {
            report(job.error().toString());
            return;
        }
        consolidation_ = job->find("job")->asString();
        report("Consolidating items…");
    }
    void poll() {
        if (consolidation_) {
            auto job = dispatcher_.jobs().status(*consolidation_);
            if (!job) {
                report(job.error().toString());
                consolidation_.reset();
            } else if (job->find("status")->asString() != "running") {
                if (job->find("status")->asString() == "done")
                    report("Items consolidated");
                else if (const auto* error = job->find("error"))
                    report(json::writeCanonical(*error));
                else
                    report("Consolidation cancelled");
                consolidation_.reset();
            }
        }
        if (!pending_) return;
        auto args = json::Value::object();
        args["job"] = pending_->job;
        auto job = dispatcher_.execute({"job.get", args, origin_});
        if (!job) {
            report(job.error().toString());
            pending_.reset();
            return;
        }
        const auto* state = job->find("status");
        if (!state || state->asString() == "running") return;
        auto pending = std::move(*pending_);
        pending_.reset();
        if (state->asString() != "done") {
            if (const auto* error = job->find("error"))
                report(json::writeCanonical(*error));
            else
                report("Audio hit detection cancelled");
            return;
        }
        if (pending.args.isNull()) {
            report("Audio hits detected");
            return;
        }
        if (pending.generation != app_.projectGeneration() ||
            pending.branch != app_.versions().branch()) {
            report("Audio selection changed; run the edit again");
            return;
        }
        const auto current = app_.head();
        for (const auto& name : pending.args["items"].asArray()) {
            const auto id = *model::Id::parse(name.asString());
            const auto* now = current.items.find(id);
            const auto* before = pending.session.items.find(id);
            if (!now || !before || model::toJson(*now) != model::toJson(*before)) {
                report("Audio selection changed; run the edit again");
                return;
            }
            auto a = *current.takes.find(*now->activeTake),
                 b = *pending.session.takes.find(*before->activeTake);
            a.transients.reset();
            b.transients.reset();
            if (model::toJson(a) != model::toJson(b)) {
                report("Audio take changed; run the edit again");
                return;
            }
        }
        execute("audio.quantize", pending.args);
    }
    std::function<void(const std::string&)> onStatus;

  private:
    void report(const std::string& message) {
        if (onStatus) onStatus(message);
    }
    void execute(const char* command, const json::Value& args) {
        auto result = dispatcher_.execute({command, args, origin_});
        report(result ? "Audio edit complete" : result.error().toString());
    }
    struct Pending {
        std::string job;
        json::Value args;
        model::Session session;
        std::uint64_t generation;
        std::string branch;
    };
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    const cmd::Origin origin_{cmd::Origin::Type::Ui, "gui", {}};
    std::optional<Pending> pending_;
    std::optional<std::string> consolidation_;
};
}  // namespace supple::gui
