// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "selection_edit.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace supple::gui {
namespace {

constexpr double kSilenceDb = -150.0, kTopDb = 12.0;
constexpr const char* kShared[] = {"mute",         "solo", "armed",        "monitor",
                                   "phase_invert", "mono", "record_input", "record_midi",
                                   "volume_db",    "pan"};

const model::Track* trackOf(const model::Session& s, const std::string& text) {
    const auto id = model::Id::parse(text);
    return id ? s.tracks.find(*id) : nullptr;
}

}  // namespace

Result<json::Value> SelectionEdit::set(json::Value args, const std::string& gesture) {
    cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
    origin.gesture = gesture;
    const json::Value* named = args.find("track");
    const std::string track = named && named->isString() ? named->asString() : std::string();
    const model::Session& s = app_.head();
    std::vector<std::string> targets{track};
    // Only a strip's mix and record controls spread; a name or colour does not.
    const bool shared = std::any_of(std::begin(kShared), std::end(kShared),
                                    [&](const char* k) { return args.find(k) != nullptr; });
    if (shared && ui_.selectedTracks.size() > 1 && trackSelected(ui_, track)) {
        // Arm, monitoring and the input belong to channels: busses skip them.
        const bool channelOnly = args.find("armed") || args.find("monitor") ||
                                 args.find("record_input") || args.find("record_midi");
        targets.clear();
        for (const std::string& t : ui_.selectedTracks) {
            const model::Track* tr = trackOf(s, t);
            if (!tr) continue;
            if (channelOnly && tr->type != model::StripType::Channel && t != track) continue;
            targets.push_back(t);
        }
    }
    if (targets.size() <= 1) return dispatcher_.execute({"track.set", std::move(args), origin});
    const bool level = args.find("volume_db") != nullptr, pan = args.find("pan") != nullptr;
    if (!gesture.empty() && (level || pan) && gesture != gesture_) {
        gesture_ = gesture;  // the drag's first move: where every track starts
        start_.clear();
        for (const std::string& t : targets)
            if (const model::Track* tr = trackOf(s, t)) start_[t] = {tr->volumeDb, tr->pan};
    }
    const bool relative = !gesture.empty() && gesture == gesture_ && start_.contains(track);
    return dispatcher_.transact(
        "Edit " + std::to_string(targets.size()) + " tracks", origin, [&]() -> Result<json::Value> {
            Result<json::Value> last = json::Value::object();
            for (const std::string& t : targets) {
                json::Value each = args;
                each["track"] = t;
                if (relative && start_.contains(t)) {
                    const auto [db0, pan0] = start_.at(t);
                    const auto [refDb, refPan] = start_.at(track);
                    if (level)
                        each["volume_db"] = std::clamp(db0 + (args["volume_db"].asDouble() - refDb),
                                                       kSilenceDb, kTopDb);
                    if (pan)
                        each["pan"] =
                            std::clamp(pan0 + (args["pan"].asDouble() - refPan), -100.0, 100.0);
                }
                last = dispatcher_.execute({"track.set", std::move(each), origin});
                if (!last) return last;
            }
            return last;
        });
}

}  // namespace supple::gui
