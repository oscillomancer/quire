// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "track_menu.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "supple/model/objects.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"

namespace supple::gui {

Status removeTracks(cmd::Dispatcher& dispatcher, const model::Session& session,
                    const std::vector<std::string>& tracks) {
    // Deepest first: a folder can go once its children have.
    std::vector<std::pair<int, model::Id>> order;
    for (const std::string& text : tracks) {
        const auto id = model::Id::parse(text);
        if (!id || *id == model::kMaster || !session.tracks.find(*id)) continue;
        int depth = 0;
        for (const model::Track* t = session.tracks.find(*id); t && t->parent;
             t = session.tracks.find(*t->parent))
            ++depth;
        order.emplace_back(depth, *id);
    }
    if (order.empty()) return {};
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    const cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
    auto r = dispatcher.transact("Remove tracks", origin, [&]() -> Result<json::Value> {
        for (const auto& [depth, id] : order) {
            json::Value args = json::Value::object();
            args["track"] = id.str();
            auto one = dispatcher.execute({"track.remove", std::move(args), origin});
            if (!one) return one;
        }
        return json::Value::object();
    });
    if (!r) return std::move(r).error();
    return {};
}

Result<std::vector<std::string>> duplicateTracks(cmd::Dispatcher& dispatcher,
                                                 const model::Session& session,
                                                 const std::vector<std::string>& tracks) {
    json::Value ids = json::Value::array();
    for (const std::string& text : tracks) {
        const auto id = model::Id::parse(text);
        const model::Track* t = id ? session.tracks.find(*id) : nullptr;
        if (t && t->type != model::StripType::Master) ids.push_back(text);
    }
    std::vector<std::string> copies;
    if (ids.asArray().empty()) return copies;
    json::Value args = json::Value::object();
    args["tracks"] = std::move(ids);
    auto r = dispatcher.execute(
        {"track.duplicate", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
    if (!r) return std::move(r).error();
    for (const json::Value& id : (*r)["tracks"].asArray()) copies.push_back(id.asString());
    return copies;
}

void TrackMenu::commit(std::string_view command, json::Value args) const {
    auto r = dispatcher.execute(
        {std::string(command), std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
    if (!r && onError) onError(r.error().message);
}

void TrackMenu::open(const model::Id& track, const ui::Rect& anchor,
                     std::function<void()> rename) const {
    if (!app.head().tracks.find(track)) return;
    std::vector<ui::MenuItem> items;
    ui::MenuItem m;
    m.label = "Rename";
    m.run = [this, track, anchor, rename] {
        if (rename)
            rename();
        else
            openRename(track, anchor);
    };
    items.push_back(std::move(m));
    m = {};
    m.label = "Color…";
    m.run = [this, track, anchor] { (void)openColors(track, anchor); };
    items.push_back(std::move(m));
    if (app.head().tracks.find(track)->type == model::StripType::Channel) {
        m = {};
        m.label = "Timebase…";
        m.run = [this, track, anchor] { openTimebase(track, anchor); };
        items.push_back(std::move(m));
    }
    m = {};
    m.label = "Channels…";
    m.run = [this, track, anchor] { openChannels(track, anchor); };
    items.push_back(std::move(m));
    items.push_back({"", "", {}, true, true});  // separator
    // Duplicate and Delete: the whole selection when the track is part of it.
    std::vector<std::string> doomed{track.str()};
    if (selection) {
        auto selected = selection();
        if (std::find(selected.begin(), selected.end(), track.str()) != selected.end())
            doomed = std::move(selected);
    }
    std::vector<std::string> copied = doomed;
    std::erase_if(copied, [&](const std::string& text) {
        const auto id = model::Id::parse(text);
        const model::Track* t = id ? app.head().tracks.find(*id) : nullptr;
        return !t || t->type == model::StripType::Master;
    });
    m = {};
    m.label =
        copied.size() > 1 ? "Duplicate " + std::to_string(copied.size()) + " tracks" : "Duplicate";
    m.enabled = !copied.empty();  // masters are not duplicated
    m.run = [this, copied] {
        auto r = duplicateTracks(dispatcher, app.head(), copied);
        if (!r) {
            if (onError) onError(r.error().message);
        } else if (select) {
            select(std::move(*r));
        }
    };
    items.push_back(std::move(m));
    std::erase(doomed, model::kMaster.str());
    m = {};
    m.label = doomed.size() > 1 ? "Delete " + std::to_string(doomed.size()) + " tracks" : "Delete";
    m.enabled = !doomed.empty();  // the main master stays
    m.run = [this, doomed] {
        if (auto r = removeTracks(dispatcher, app.head(), doomed); !r && onError)
            onError(r.error().message);
    };
    items.push_back(std::move(m));
    root.openPopup(
        ui::makeMenu("track.menu." + track.str(), std::move(items), [this] { root.closePopup(); }),
        anchor);
}

Status TrackMenu::openColors(const model::Id& track, const ui::Rect& anchor) const {
    const model::Track* tr = app.head().tracks.find(track);
    if (!tr) return Error("track.unknown", "no track " + track.str(), "/track", "use track.list");
    // The whole selection when the track is part of it, as one undo step.
    std::vector<std::string> tracks{track.str()};
    if (selection) {
        auto selected = selection();
        if (std::find(selected.begin(), selected.end(), track.str()) != selected.end())
            tracks = std::move(selected);
    }
    std::vector<ui::MenuItem> items;
    auto pick = [this, tracks](std::string colour) {
        const cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
        auto r = dispatcher.transact("Track color", origin, [&]() -> Result<json::Value> {
            for (const std::string& id : tracks) {
                const auto parsed = model::Id::parse(id);
                if (!parsed || !app.head().tracks.find(*parsed)) continue;  // gone meanwhile
                json::Value args = json::Value::object();
                args["track"] = id;
                args["color"] = colour;
                auto one = dispatcher.execute({"track.set", std::move(args), origin});
                if (!one) return one;
            }
            return json::Value::object();
        });
        if (!r && onError) onError(r.error().message);
    };
    for (std::string_view name : model::kTrackColors) {
        ui::MenuItem m;
        m.label = std::string(name);
        m.label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(m.label[0])));
        m.checked = tr->color == name;
        m.swatch = root.theme().trackColor(name);
        m.run = [pick, colour = std::string(name)] { pick(colour); };
        items.push_back(std::move(m));
    }
    items.push_back({"", "", {}, true, true});  // separator
    ui::MenuItem none;
    none.label = "None";
    none.checked = tr->color.empty();
    none.run = [pick] { pick(""); };
    items.push_back(std::move(none));
    root.openPopup(
        ui::makeMenu("track.color." + track.str(), std::move(items), [this] { root.closePopup(); }),
        anchor, anchor.w > 0 ? ui::Placement::Below : ui::Placement::Center);
    return {};
}

void TrackMenu::openChannels(const model::Id& track, const ui::Rect& anchor) const {
    const model::Track* tr = app.head().tracks.find(track);
    if (!tr) return;
    std::vector<ui::MenuItem> items;
    std::vector<std::pair<std::string, int>> counts = {
        {"Mono (1)", 1},      {"Stereo (2)", 2},    {"4: aux 3/4", 4},
        {"6: aux 3-6", 6},    {"8: aux 3-8", 8},    {"16: aux 3-16", 16},
        {"32: aux 3-32", 32}, {"64: aux 3-64", 64}, {"128: aux 3-128", 128}};
    // A count set elsewhere (a multi-out instrument's, ADR-0021) shows too.
    if (std::none_of(counts.begin(), counts.end(),
                     [&](const auto& c) { return c.second == tr->channels; })) {
        counts.emplace_back(
            std::to_string(tr->channels) + ": aux 3-" + std::to_string(tr->channels), tr->channels);
        std::sort(counts.begin(), counts.end(),
                  [](const auto& x, const auto& y) { return x.second < y.second; });
    }
    for (const auto& [label, count] : counts) {
        ui::MenuItem m;
        m.label = label;
        m.checked = tr->channels == count;
        m.enabled = count >= 2 || tr->type == model::StripType::Channel;
        m.run = [this, track, count] {
            json::Value args = json::Value::object();
            args["track"] = track.str();
            args["channels"] = static_cast<std::int64_t>(count);
            commit("track.set", std::move(args));
        };
        items.push_back(std::move(m));
    }
    root.openPopup(ui::makeMenu("track.channels." + track.str(), std::move(items),
                                [this] { root.closePopup(); }),
                   anchor, anchor.w > 0 ? ui::Placement::Below : ui::Placement::Center);
}

void TrackMenu::openTimebase(const model::Id& track, const ui::Rect& anchor) const {
    const model::Track* tr = app.head().tracks.find(track);
    if (!tr) return;
    std::vector<ui::MenuItem> items;
    for (auto [label, value, name] :
         {std::tuple{"Follow the project", model::TrackTimebase::Project, "project"},
          {"Follow the beat", model::TrackTimebase::Beats, "beats"},
          {"Follow time", model::TrackTimebase::Time, "time"},
          {"Follow the beat (start only)", model::TrackTimebase::BeatsPosition,
           "beats_position"}}) {
        ui::MenuItem m;
        m.label = label;
        m.checked = tr->timebase == value;
        m.run = [this, track, timebase = std::string(name)] {
            json::Value args = json::Value::object();
            args["track"] = track.str();
            args["timebase"] = timebase;
            commit("track.set", std::move(args));
        };
        items.push_back(std::move(m));
    }
    root.openPopup(ui::makeMenu("track.timebase." + track.str(), std::move(items),
                                [this] { root.closePopup(); }),
                   anchor, anchor.w > 0 ? ui::Placement::Below : ui::Placement::Center);
}

void TrackMenu::openRename(const model::Id& track, const ui::Rect& anchor) const {
    const model::Track* tr = app.head().tracks.find(track);
    if (!tr) return;
    auto field = std::make_unique<ui::TextField>("track.rename." + track.str(), tr->name);
    ui::TextField* f = field.get();
    f->layout.width = ui::SizeRule::fixed(180);
    f->layout.height = ui::SizeRule::fixed(24);
    f->onSubmit = [this, track](const std::string& text) {
        json::Value args = json::Value::object();
        args["track"] = track.str();
        args["name"] = text;  // a copy: closing destroys the field and its text
        ui::Root& r = root;
        commit("track.set", std::move(args));
        r.closePopup();  // last: it destroys this lambda's field
    };
    f->onCancel = [this] { root.closePopup(); };
    root.openPopup(std::move(field), anchor);
    f->selectAll();
    root.focus(f);
}

}  // namespace supple::gui
