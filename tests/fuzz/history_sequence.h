// Shared byte-stream history property: libFuzzer mutates the bytes, while
// engine.tests feeds fixed seeded streams through exactly the same commands.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <supple/engine/app.h>
#include <supple/model/schemas.h>

namespace supple::test {

inline Status historySequence(engine::EngineApp& app, cmd::Dispatcher& bus,
                              std::span<const std::uint8_t> bytes) {
    const cmd::Origin origin{cmd::Origin::Type::Test, "history-property", ""};
    const std::string initial = model::serializeProject(app.head());
    std::map<std::string, std::string> bases{{"main", initial}};
    unsigned branches = 0;
    const auto ids = [&](const auto& map, std::uint8_t choice) -> std::string {
        if (map.empty()) return "tr_99999";  // deliberately invalid reference
        auto it = map.begin();
        std::advance(it, choice % map.size());
        return it->id.str();
    };
    const auto fail = [](std::size_t step, std::string message) -> Status {
        return Error("history.property", "step " + std::to_string(step) + ": " + message);
    };
    for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 96); ++i) {
        const std::uint8_t byte = bytes[i];
        const model::Session& s = app.head();
        json::Value args = json::Value::object();
        std::string command;
        switch (byte % 19) {
            case 0:
                command = "track.add";
                args["name"] = "Track " + std::to_string(i);
                args["type"] = (byte & 0x80) ? "master" : (byte & 0x40) ? "buss" : "channel";
                if ((byte & 0x20) && !(byte & 0x80)) args["parent"] = ids(s.tracks, byte >> 2);
                break;
            case 1:
                command = "track.set";
                args["track"] = ids(s.tracks, byte >> 2);
                args["volume_db"] = static_cast<double>(static_cast<int>(byte >> 3) - 15);
                args["mute"] = bool(byte & 0x20);
                break;
            case 2:
                command = "track.remove";
                args["track"] = ids(s.tracks, byte >> 2);
                break;
            case 3:
                command = "fx.add";
                args["track"] = ids(s.tracks, byte >> 2);
                args["plugin"] =
                    (byte & 0x20) ? "com.suppleaudio.test.gain" : "com.suppleaudio.test.sine";
                break;
            case 4:
                command = "fx.set";
                args["fx"] = ids(s.fx, byte >> 2);
                args["bypass"] = bool(byte & 0x20);
                args["wet"] = static_cast<double>(byte % 101);
                break;
            case 5:
                command = "fx.remove";
                args["fx"] = ids(s.fx, byte >> 2);
                break;
            case 6:
                command = "item.add";
                args["track"] = ids(s.tracks, byte >> 2);
                args["bar"] = 1 + (byte >> 6);
                args["notation"] = "voice lead; 1: C4:q D4:q E4:q G4:q";
                break;
            case 7:
                command = "item.move";
                args["item"] = ids(s.items, byte >> 2);
                args["position"] = static_cast<double>(byte >> 4);
                break;
            case 8:
                command = "item.remove";
                args["item"] = ids(s.items, byte >> 2);
                break;
            case 9:
                command = "midi.write";
                args["item"] = ids(s.items, byte >> 2);
                args["notation"] = "voice lead; 1: G4:q E4:q D4:q C4:q";
                break;
            case 10:
                command = "env.add";
                args["track"] = ids(s.tracks, byte >> 2);
                args["param"] = "volume";
                break;
            case 11: {
                command = "env.points.set";
                args["env"] = ids(s.envelopes, byte >> 2);
                json::Value points = json::Value::array();
                json::Value a = json::Value::object(), b = json::Value::object();
                a["beat"] = 0;
                a["value"] = -6;
                b["beat"] = 4;
                b["value"] = static_cast<int>(byte >> 4) - 12;
                points.push_back(std::move(a));
                points.push_back(std::move(b));
                args["points"] = std::move(points);
                break;
            }
            case 12:
                command = "env.remove";
                args["env"] = ids(s.envelopes, byte >> 2);
                break;
            case 13: {
                command = "batch";
                auto parsed = json::parse(
                    R"({"steps":[{"command":"track.add","args":{"name":"Batch"}},{"command":"track.set","args":{"track":"tr_0","pan":10}}]})");
                args = *parsed;
                if (byte & 0x20) args["steps"].asArray()[1]["args"]["track"] = "tr_99999";
                break;
            }
            case 14:
                command = "history.undo";
                break;
            case 15:
                command = "history.redo";
                break;
            case 16:
                command = "branch.create";
                if (branches < 3) {
                    const std::string name = "fuzz/" + std::to_string(branches++);
                    bases[name] = model::serializeProject(app.head());
                    args["name"] = name;
                } else {
                    args["name"] = "fuzz/0";  // duplicate: must fail atomically
                }
                break;
            case 17:
                command = "branch.switch";
                args["name"] = (byte & 0x20) && branches ? "fuzz/0" : "main";
                break;
            case 18:
                command = "branch.merge";
                args["from"] = branches ? "fuzz/0" : "unknown";
                break;
        }
        std::map<std::string, std::pair<std::uint64_t, std::string>> before;
        for (const auto& branch : app.versions().branches())
            before.emplace(branch,
                           std::make_pair(app.versions().headVersion(branch),
                                          model::serializeProject(*app.versions().head(branch))));
        const std::string priorBranch = app.versions().branch();
        auto result = bus.execute({command, std::move(args), origin});
        if (!result) {
            if (priorBranch != app.versions().branch() ||
                before.size() != app.versions().branches().size())
                return fail(i, "failed " + command + " changed the branches");
            for (const auto& [branch, snapshot] : before)
                if (snapshot.first != app.versions().headVersion(branch) ||
                    snapshot.second != model::serializeProject(*app.versions().head(branch)))
                    return fail(i, "failed " + command + " changed " + branch);
        }
        for (const auto& branch : app.versions().branches()) {
            auto check = model::checkInvariants(*app.versions().head(branch));
            if (!check) return fail(i, branch + ": " + check.error().toString());
        }
        // Probe inverse navigation after each successful edit. The later
        // full unwind also checks the whole branch's history at once.
        if (result && app.versions().branch() == priorBranch && app.versions().canUndo() &&
            command != "history.undo" && command != "history.redo") {
            const std::string after = model::serializeProject(app.head());
            if (!app.versions().undo() || !app.versions().redo() ||
                model::serializeProject(app.head()) != after)
                return fail(i, "undo/redo changed " + command);
        }
    }
    const std::string selected = app.versions().branch();
    const std::string final = model::serializeProject(app.head());
    for (const auto& branch : app.versions().branches()) {
        if (auto st = app.switchBranch(branch); !st) return st;
        const std::string tip = model::serializeProject(app.head());
        std::size_t undone = 0;
        while (app.versions().canUndo()) {
            if (!app.versions().undo()) return fail(bytes.size(), "undo failed");
            ++undone;
        }
        if (model::serializeProject(app.head()) != bases.at(branch))
            return fail(bytes.size(), "undo-all missed " + branch + " base");
        for (std::size_t n = 0; n < undone; ++n)
            if (!app.versions().redo()) return fail(bytes.size(), "redo failed");
        if (model::serializeProject(app.head()) != tip)
            return fail(bytes.size(), "redo-all changed " + branch);
    }
    if (auto st = app.switchBranch(selected); !st) return st;
    if (auto st = app.save("history-fuzz.project"); !st) return st;
    if (auto st = app.open("history-fuzz.project"); !st) return st;
    if (model::serializeProject(app.head()) != final)
        return fail(bytes.size(), "save/open changed project text");
    return model::checkInvariants(app.head());
}

}  // namespace supple::test
