#include "track_menu.h"

#include <cctype>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "supple/model/objects.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"

namespace supple::gui {

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
    m = {};
    m.label = "Delete";
    m.enabled = track != model::kMaster;  // the main master stays
    m.run = [this, track] {
        json::Value args = json::Value::object();
        args["track"] = track.str();
        commit("track.remove", std::move(args));
    };
    items.push_back(std::move(m));
    root.openPopup(
        ui::makeMenu("track.menu." + track.str(), std::move(items), [this] { root.closePopup(); }),
        anchor);
}

Status TrackMenu::openColors(const model::Id& track, const ui::Rect& anchor) const {
    const model::Track* tr = app.head().tracks.find(track);
    if (!tr) return Error("track.unknown", "no track " + track.str(), "/track", "use track.list");
    std::vector<ui::MenuItem> items;
    auto pick = [this, track](std::string colour) {
        json::Value args = json::Value::object();
        args["track"] = track.str();
        args["color"] = std::move(colour);
        commit("track.set", std::move(args));
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
    for (auto [label, count] : {std::pair{"Mono (1)", 1},
                                {"Stereo (2)", 2},
                                {"4: aux 3/4", 4},
                                {"6: aux 3-6", 6},
                                {"8: aux 3-8", 8}}) {
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
