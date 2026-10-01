#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cstdlib>

#include "supple/actions/actions.h"
#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::actions {
namespace {

constexpr unsigned kShift = 1, kCtrl = 2, kAlt = 4, kSuper = 8;

json::Value bindingJson(const Binding& b) {
    json::Value o = json::Value::object();
    json::Value keys = json::Value::array();
    for (const Chord& c : b.keys) keys.push_back(describe(c));
    o["keys"] = std::move(keys);
    o["section"] = b.section;
    o["action"] = b.action;
    return o;
}

Result<Binding> bindingFrom(const json::Value& v, const std::string& path) {
    const json::Value* keys = v.isObject() ? v.find("keys") : nullptr;
    const json::Value* section = v.isObject() ? v.find("section") : nullptr;
    const json::Value* action = v.isObject() ? v.find("action") : nullptr;
    if (!keys || !keys->isArray() || keys->asArray().empty() || keys->asArray().size() > 2 ||
        !action || !action->isString())
        return Error(
            "keymap.binding",
            "a binding is {\"keys\": [\"Ctrl+S\"], \"section\": \"global\", \"action\": \"…\"}",
            path);
    Binding b;
    for (const json::Value& k : keys->asArray()) {
        auto chord = k.isString() ? parseChord(k.asString()) : std::nullopt;
        if (!chord)
            return Error("keymap.key", "unknown key " + (k.isString() ? k.asString() : "?"),
                         path + "/keys");
        b.keys.push_back(*chord);
    }
    b.section = section && section->isString() ? section->asString() : "global";
    b.action = action->asString();
    return b;
}

bool startsWith(const std::vector<Chord>& keys, const std::vector<Chord>& prefix) {
    return keys.size() > prefix.size() && std::equal(prefix.begin(), prefix.end(), keys.begin());
}

}  // namespace

std::string describe(const Chord& c) {
    std::string out;
    if (c.mods & kCtrl) out += "Ctrl+";
    if (c.mods & kAlt) out += "Alt+";
    if (c.mods & kShift) out += "Shift+";
    if (c.mods & kSuper) out += "Super+";
    const char* name = SDL_GetKeyName(static_cast<SDL_Keycode>(c.key));
    out += (name && *name) ? name : "?";
    return out;
}

std::optional<Chord> parseChord(std::string_view text) {
    Chord c;
    std::string rest(text);
    for (;;) {
        auto take = [&](std::string_view prefix, unsigned bit) {
            if (rest.size() > prefix.size() && rest.compare(0, prefix.size(), prefix) == 0) {
                c.mods |= bit;
                rest.erase(0, prefix.size());
                return true;
            }
            return false;
        };
        if (!(take("Ctrl+", kCtrl) || take("Alt+", kAlt) || take("Shift+", kShift) ||
              take("Super+", kSuper)))
            break;
    }
    const SDL_Keycode key = SDL_GetKeyFromName(rest.c_str());
    if (key == SDLK_UNKNOWN) return std::nullopt;
    c.key = static_cast<int>(key);
    return c;
}

Keymap Keymap::defaults() {
    Keymap k;
    auto add = [&](std::initializer_list<const char*> keys, const char* section,
                   const char* action) {
        Binding b;
        for (const char* key : keys) b.keys.push_back(*parseChord(key));
        b.section = section;
        b.action = action;
        k.bindings_.push_back(b);
    };
    // Close to the common DAW keys (key choices, not copied assets).
    add({"Space"}, "global", "transport.toggle");
    add({"Ctrl+R"}, "global", "transport.record");
    add({"R"}, "global", "transport.loop.toggle");
    add({"Home"}, "global", "transport.seek:start");
    add({"End"}, "global", "transport.seek:end");
    add({"Ctrl+Z"}, "global", "history.undo");
    add({"Ctrl+Shift+Z"}, "global", "history.redo");
    add({"Ctrl+Y"}, "global", "history.redo");
    add({"Ctrl+S"}, "global", "ui.file.save");
    add({"Ctrl+Shift+S"}, "global", "ui.file.save_as");
    add({"Ctrl+O"}, "global", "ui.file.open");
    add({"Ctrl+N"}, "global", "ui.file.new");
    add({"Ctrl+I"}, "global", "ui.file.import");
    add({"Ctrl+E"}, "global", "ui.midi.edit");
    add({"Alt+R"}, "global", "ui.routing");
    add({"Ctrl+M"}, "global", "ui.mixer");
    add({"Ctrl+H"}, "global", "ui.history");
    add({"Ctrl+Alt+R"}, "global", "ui.render");
    add({"Ctrl+T"}, "global", "track.add:channel");
    add({"Ctrl+Alt+T"}, "global", "track.add:buss");
    add({"Shift+/"}, "global", "ui.actions");
    add({"Ctrl+Shift+P"}, "global", "ui.actions");
    add({"Ctrl+Shift+A"}, "global", "ui.agent");
    add({"S"}, "arrange", "item.split:playhead");
    add({"M"}, "arrange", "ui.marker.add");
    add({"Shift+R"}, "arrange", "ui.region.add");
    add({"Delete"}, "arrange", "item.remove:selected");
    k.defaults_ = k.bindings_;
    return k;
}

std::filesystem::path Keymap::defaultFile(std::string_view slug) {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / slug / "keymap.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / slug / "keymap.json";
    return {};
}

Result<Keymap> Keymap::load(const std::filesystem::path& file) {
    Keymap k = defaults();
    if (file.empty() || !std::filesystem::exists(file)) return k;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* schema = doc->isObject() ? doc->find("schema") : nullptr;
    if (!schema || !schema->isString() || schema->asString() != "supple.keymap/1.0")
        return Error("keymap.schema", file.string() + " is not a supple.keymap/1.0 document",
                     "/schema");
    for (const char* list : {"unbind", "bind"}) {
        const json::Value* items = doc->find(list);
        if (!items) continue;
        if (!items->isArray())
            return Error("keymap.list", std::string(list) + " must be an array",
                         std::string("/") + list);
        for (std::size_t i = 0; i < items->asArray().size(); ++i) {
            auto b =
                bindingFrom(items->asArray()[i], std::string("/") + list + "/" + std::to_string(i));
            if (!b) return std::move(b).error();
            if (std::string_view(list) == "bind")
                k.bind(*b);
            else
                k.unbind(*b);
        }
    }
    return k;
}

Status Keymap::save(const std::filesystem::path& file) const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.keymap/1.0";
    json::Value bind = json::Value::array(), unbind = json::Value::array();
    for (const Binding& b : bindings_)
        if (std::find(defaults_.begin(), defaults_.end(), b) == defaults_.end())
            bind.push_back(bindingJson(b));
    for (const Binding& b : defaults_)
        if (std::find(bindings_.begin(), bindings_.end(), b) == bindings_.end())
            unbind.push_back(bindingJson(b));
    doc["bind"] = std::move(bind);
    doc["unbind"] = std::move(unbind);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}

void Keymap::bind(Binding b) {
    std::erase_if(bindings_,
                  [&](const Binding& o) { return o.keys == b.keys && o.section == b.section; });
    bindings_.push_back(std::move(b));
}

void Keymap::unbind(const Binding& b) { std::erase(bindings_, b); }

std::vector<Binding> Keymap::bindingsFor(std::string_view action) const {
    std::vector<Binding> out;
    for (const Binding& b : bindings_)
        if (b.action == action) out.push_back(b);
    return out;
}

std::vector<std::pair<Binding, Binding>> Keymap::conflicts() const {
    std::vector<std::pair<Binding, Binding>> out;
    for (std::size_t i = 0; i < bindings_.size(); ++i)
        for (std::size_t j = i + 1; j < bindings_.size(); ++j) {
            const Binding &a = bindings_[i], &b = bindings_[j];
            if (a.section != b.section) continue;
            // Equal keys, or one a prefix of the other (the longer can never fire).
            if (a.keys == b.keys || startsWith(a.keys, b.keys) || startsWith(b.keys, a.keys))
                out.emplace_back(a, b);
        }
    return out;
}

std::optional<std::string> Keymap::press(const Chord& chord,
                                         const std::vector<std::string>& sections) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        held_.push_back(chord);
        for (const std::string& section : sections) {
            for (const Binding& b : bindings_)
                if (b.section == section && b.keys == held_) {
                    held_.clear();
                    return b.action;
                }
            for (const Binding& b : bindings_)
                if (b.section == section && startsWith(b.keys, held_))
                    return std::nullopt;  // wait for more
        }
        const bool wasSequence = held_.size() > 1;
        held_.clear();
        if (!wasSequence) break;  // retry the chord on its own once
    }
    return std::nullopt;
}

}  // namespace supple::actions
