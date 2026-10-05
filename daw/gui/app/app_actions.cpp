#include "app_actions.h"
#include "track_menu.h"

#include <algorithm>
#include <cstdio>

#include "supple/json/parse.h"
#include "supple/schema/schema.h"
#include "supple/ui/node.h"

namespace supple::gui {

void selectTrack(UiState& ui, const std::string& track, unsigned mods,
                 const std::vector<std::string>& order) {
    auto& sel = ui.selectedTracks;
    ui.tracksFocused = true;
    if (mods & ui::kShift) {  // extend from the anchor, in the view's order
        const std::string from = ui.trackAnchor.value_or(track);
        auto a = std::find(order.begin(), order.end(), from);
        auto b = std::find(order.begin(), order.end(), track);
        if (a != order.end() && b != order.end()) {
            if (a > b) std::swap(a, b);
            sel.assign(a, b + 1);
        } else {
            sel = {track};
        }
        ui.trackAnchor = from;
    } else if (mods & ui::kCtrl) {  // toggle one
        const auto it = std::find(sel.begin(), sel.end(), track);
        if (it == sel.end())
            sel.push_back(track);
        else
            sel.erase(it);
        ui.trackAnchor = track;
    } else {
        sel = {track};
        ui.trackAnchor = track;
    }
}

bool trackSelected(const UiState& ui, const std::string& track) {
    return std::find(ui.selectedTracks.begin(), ui.selectedTracks.end(), track) !=
           ui.selectedTracks.end();
}
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

actions::Action bound(std::string id, std::string name, std::string command, json::Value args) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.command = std::move(command);
    a.args = std::move(args);
    return a;
}

json::Value obj(std::initializer_list<std::pair<const char*, json::Value>> fields) {
    json::Value o = json::Value::object();
    for (const auto& [k, v] : fields) o[k] = v;
    return o;
}

struct Selection {
    std::vector<std::string> tracks, items;
};

}  // namespace

void registerGuiCommands(cmd::Registry& registry, const UiState& ui) {
    cmd::Command<cmd::NoArgs, Selection> c;
    c.name = "gui.selection";
    c.kind = cmd::Kind::Query;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "The GUI's selected tracks and items (context only).";
    c.description =
        "Useful context for requests like \"the selected items\"; never rely on it for "
        "correctness.";
    c.tags = {"gui"};
    c.args = schema::object<cmd::NoArgs>("no_args").build();
    c.result = schema::object<Selection>("selection")
                   .field("tracks", &Selection::tracks, schema::arrayOf(schema::string()))
                   .field("items", &Selection::items, schema::arrayOf(schema::string()))
                   .build();
    c.examples = {{"Read the selection", json::Value::object(),
                   *json::parse(R"({"tracks": [], "items": []})"), "empty"}};
    c.run = [&ui](const cmd::NoArgs&, cmd::Context&) -> Result<Selection> {
        return Selection{ui.selectedTracks, ui.selectedItems};
    };
    registry.add(std::move(c));
}

AppActions::AppActions(const cmd::Registry& registry, cmd::Dispatcher& dispatcher,
                       engine::EngineApp& app, UiState& ui, std::filesystem::path keymapFile)
    : keymapFile_(std::move(keymapFile)),
      dispatcher_(dispatcher),
      app_(app),
      ui_(ui),
      runner_(registry_, dispatcher) {
    registry_.addCommands(registry);
    auto exec = [this](const char* command, json::Value args) {
        return dispatcher_.execute({command, std::move(args), kOrigin});
    };
    actions::Action toggle;
    toggle.id = "transport.toggle";
    toggle.name = "Transport: play/stop";
    toggle.description = "Start playback, or stop it when playing.";
    toggle.builtin = [exec](const actions::Context&) -> Result<json::Value> {
        auto t = exec("transport.get", json::Value::object());
        if (!t) return t;
        return exec((*t)["playing"].asBool() ? "transport.stop" : "transport.play",
                    json::Value::object());
    };
    registry_.add(toggle);
    actions::Action loop;
    loop.id = "transport.loop.toggle";
    loop.name = "Transport: toggle loop";
    loop.builtin = [exec](const actions::Context&) -> Result<json::Value> {
        auto t = exec("transport.get", json::Value::object());
        if (!t) return t;
        return exec("transport.loop", obj({{"enabled", !(*t)["loop"]["enabled"].asBool()}}));
    };
    registry_.add(loop);
    registry_.add(bound("transport.seek:start", "Transport: go to start", "transport.seek",
                        obj({{"seconds", 0.0}})));
    actions::Action end;
    end.id = "transport.seek:end";
    end.name = "Transport: go to end";
    end.builtin = [this, exec](const actions::Context&) -> Result<json::Value> {
        double last = 0;
        const model::Session& s = app_.head();
        for (const auto& e : s.items) last = std::max(last, model::itemSpan(s, *e.value).endSec);
        return exec("transport.seek", obj({{"seconds", last}}));
    };
    registry_.add(end);
    for (const char* type : {"channel", "buss", "master"})
        registry_.add(bound(std::string("track.add:") + type, std::string("Track: add ") + type,
                            "track.add", obj({{"type", type}})));
    registry_.add(bound("item.split:playhead", "Item: split selected at playhead", "item.split",
                        obj({{"item", "$each.item"}, {"at", "$playhead.beats"}})));
    registry_.add(bound("item.remove:selected", "Item: remove selected", "item.remove",
                        obj({{"item", "$each.item"}})));
    actions::Action removeTracks;
    removeTracks.id = "track.remove:selected";
    removeTracks.name = "Track: remove selected";
    removeTracks.description = "Remove every selected track (not the main master), one undo step.";
    removeTracks.builtin = [this](const actions::Context&) -> Result<json::Value> {
        if (ui_.selectedTracks.empty()) return Error("action.selection", "select tracks first");
        SUPPLE_TRY(gui::removeTracks(dispatcher_, app_.head(), ui_.selectedTracks));
        return json::Value::object();
    };
    registry_.add(removeTracks);
    // Delete: what was selected last, tracks (TCP, mixer) or items (arrange).
    actions::Action remove;
    remove.id = "edit.delete";
    remove.name = "Edit: delete the selection";
    remove.description =
        "Remove the selected tracks when tracks were selected last, else the "
        "selected items.";
    remove.builtin = [this](const actions::Context&) -> Result<json::Value> {
        return run(ui_.tracksFocused && !ui_.selectedTracks.empty() ? "track.remove:selected"
                                                                    : "item.remove:selected");
    };
    registry_.add(remove);
    for (auto [name, label] : {std::pair{"beats", "Project: items follow the beat"},
                               {"time", "Project: items follow time"},
                               {"beats_position", "Project: items follow the beat (start only)"}})
        registry_.add(bound(std::string("project.timebase:") + name, label, "project.set",
                            obj({{"timebase", name}})));

    auto loaded = actions::Keymap::load(keymapFile_);
    if (loaded) {
        keymap_ = std::move(*loaded);
    } else {
        std::fprintf(stderr, "keymap: %s (using the defaults)\n",
                     loaded.error().toString().c_str());
        keymap_ = actions::Keymap::defaults();
    }
}

void AppActions::addUi(std::string id, std::string name, std::function<void()> run) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.builtin = [run = std::move(run)](const actions::Context&) -> Result<json::Value> {
        run();
        return json::Value::object();
    };
    registry_.add(std::move(a));
}

void AppActions::addUi(std::string id, std::string name, std::string prompt,
                       std::function<Status(const json::Value& value)> run) {
    actions::Action a;
    a.id = std::move(id);
    a.name = std::move(name);
    a.prompts = {prompt};
    a.builtin = [prompt, run = std::move(run)](const actions::Context& c) -> Result<json::Value> {
        const json::Value* v = c.prompted.find(prompt);
        auto r = run(v ? *v : json::Value());
        if (!r) return std::move(r).error();
        return json::Value::object();
    };
    registry_.add(std::move(a));
}

actions::Context AppActions::context(std::vector<std::string> sections) const {
    actions::Context c;
    c.selectedTracks = ui_.selectedTracks;
    c.selectedItems = ui_.selectedItems;
    c.sections = std::move(sections);
    if (auto t = app_.transport("get")) {
        c.playheadSeconds = t->seconds;
        c.playheadBeats = t->beat;
    }
    return c;
}

Result<json::Value> AppActions::run(const actions::Action& action, const json::Value& prompted) {
    auto r = runner_.run(action, context(), prompted);
    if (!r && onError) onError(action.name + ": " + r.error().message);
    return r;
}

Result<json::Value> AppActions::run(std::string_view id) {
    const actions::Action* a = registry_.find(id);
    if (!a) return Error("action.unknown", "no action " + std::string(id));
    return run(*a);
}

bool AppActions::key(int keycode, unsigned mods, bool typing) {
    if (typing && !(mods & (2u | 4u | 8u)) && !keymap_.pending()) return false;  // plain keys type
    const bool wasPending = keymap_.pending();
    const auto action = keymap_.press({keycode, mods}, {"arrange", "global"});
    if (action) {
        if (const actions::Action* a = registry_.find(*action))
            (void)run(*a);
        else if (onError)
            onError("no action " + *action + " (bound to a key)");
        return true;
    }
    return keymap_.pending() || wasPending;
}

std::string AppActions::keysFor(std::string_view action) const {
    const auto bindings = keymap_.bindingsFor(action);
    if (bindings.empty()) return {};
    std::string out;
    for (const actions::Chord& c : bindings.front().keys)
        out += (out.empty() ? "" : " ") + actions::describe(c);
    return out;
}

}  // namespace supple::gui
