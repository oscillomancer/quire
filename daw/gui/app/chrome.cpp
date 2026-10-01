#include "chrome.h"

#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/ui/popup.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

// A flat menu-bar title: opens its menu on press.
class MenuTitle : public ui::Node {
  public:
    MenuTitle(std::string title, std::function<void(const ui::Rect&)> open)
        : Node("menu." + title, ui::Role::Menu), title_(std::move(title)), open_(std::move(open)) {
        interactive = true;
        layout.width = ui::SizeRule::content();
    }
    ui::Size measure(const ui::Canvas& c, ui::Size) override {
        return {c.textWidth(title_, ui::Font::Ui, c.theme().type("small")) + 20, 28};
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        if (hovered()) c.fillRound(bounds().inset(2), t.radius("s"), t.color("raised"));
        c.text(bounds(), title_, ui::Font::Ui, t.type("small"), t.color("ink"),
               ui::kCenter | ui::kMiddle);
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            open_(bounds());
            return true;
        }
        return e.type == ui::PointerEvent::Type::Up;
    }
    std::string valueText() const override { return title_; }

  private:
    std::string title_;
    std::function<void(const ui::Rect&)> open_;
};

class ChromeBar : public ui::Node {
  public:
    using Node::Node;
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
    }
};

std::string shortName(const actions::Action& a) {
    const auto colon = a.name.find(": ");
    std::string s = colon == std::string::npos ? a.name : a.name.substr(colon + 2);
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

}  // namespace

ChromeLayout ChromeLayout::defaults() {
    ChromeLayout l;
    l.menus = {
        {"File",
         {"ui.file.new", "ui.file.open", "ui.file.save", "ui.file.save_as", "-", "ui.file.import",
          "-", "render.mix", "ui.render", "-", "ui.quit"}},
        {"Edit", {"history.undo", "history.redo", "-", "ui.actions"}},
        {"View", {"ui.actions", "ui.mixer", "ui.routing", "ui.history", "ui.agent"}},
        {"Track", {"track.add:channel", "track.add:buss", "track.add:master", "-", "ui.fx.chain"}},
        {"Item",
         {"ui.midi.new", "item.split:playhead", "item.remove:selected", "-", "ui.midi.edit"}},
        {"Transport",
         {"transport.toggle", "transport.record", "transport.loop.toggle", "-",
          "transport.seek:start", "transport.seek:end"}},
    };
    l.toolbar = {"transport.toggle", "transport.record", "transport.loop.toggle",
                 "history.undo",     "history.redo",     "track.add:channel",
                 "ui.actions"};
    return l;
}

std::filesystem::path ChromeLayout::defaultFile(std::string_view slug) {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / slug / "menus.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / slug / "menus.json";
    return {};
}

Result<ChromeLayout> ChromeLayout::load(const std::filesystem::path& file) {
    ChromeLayout l = defaults();
    if (file.empty() || !std::filesystem::exists(file)) return l;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* schema = doc->isObject() ? doc->find("schema") : nullptr;
    if (!schema || !schema->isString() || schema->asString() != "supple.menus/1.0")
        return Error("menus.schema", file.string() + " is not a supple.menus/1.0 document",
                     "/schema");
    auto strings = [](const json::Value& v,
                      const std::string& path) -> Result<std::vector<std::string>> {
        if (!v.isArray()) return Error("menus.list", "expected an array of action ids", path);
        std::vector<std::string> out;
        for (const json::Value& s : v.asArray()) {
            if (!s.isString()) return Error("menus.item", "expected an action id", path);
            out.push_back(s.asString());
        }
        return out;
    };
    if (const json::Value* menus = doc->find("menus")) {
        if (!menus->isArray()) return Error("menus.list", "menus must be an array", "/menus");
        l.menus.clear();
        for (std::size_t i = 0; i < menus->asArray().size(); ++i) {
            const json::Value& m = menus->asArray()[i];
            const std::string path = "/menus/" + std::to_string(i);
            const json::Value* title = m.isObject() ? m.find("title") : nullptr;
            const json::Value* items = m.isObject() ? m.find("items") : nullptr;
            if (!title || !title->isString() || !items)
                return Error("menus.menu", "a menu is {title, items}", path);
            auto ids = strings(*items, path + "/items");
            if (!ids) return std::move(ids).error();
            l.menus.push_back({title->asString(), std::move(*ids)});
        }
    }
    if (const json::Value* toolbar = doc->find("toolbar")) {
        auto ids = strings(*toolbar, "/toolbar");
        if (!ids) return std::move(ids).error();
        l.toolbar = std::move(*ids);
    }
    return l;
}

std::unique_ptr<ui::Node> makeChrome(ui::Root& root, AppActions& actions, ChromeLayout layout) {
    auto bar = std::make_unique<ChromeBar>("chrome");
    bar->layout.dir = ui::Layout::Dir::Row;
    bar->layout.height = ui::SizeRule::fixed(36);
    bar->layout.padding = ui::Insets::xy(8, 4);
    bar->layout.gap = 2;
    bar->layout.align = ui::Layout::Align::Center;
    for (const ChromeLayout::Menu& menu : layout.menus) {
        auto items = menu.items;
        bar->emplace<MenuTitle>(
            menu.title, [&root, &actions, items, title = menu.title](const ui::Rect& anchor) {
                std::vector<ui::MenuItem> entries;
                for (const std::string& id : items) {
                    if (id == "-") {
                        entries.push_back({"", "", {}, true, true});
                        continue;
                    }
                    const actions::Action* a = actions.registry().find(id);
                    if (!a) continue;  // unknown ids in a user file are skipped
                    const std::string name = shortName(*a) + (a->prompts.empty() ? "" : "…");
                    entries.push_back(
                        {name, actions.keysFor(id), [&actions, id] { (void)actions.run(id); }});
                }
                root.openPopup(ui::makeMenu("menu." + title + ".items", std::move(entries),
                                            [&root] { root.closePopup(); }),
                               anchor);
            });
    }
    auto& gap = bar->emplace<ui::Node>("chrome.gap");
    gap.layout.width = ui::SizeRule::fixed(16);
    for (const std::string& id : layout.toolbar) {
        const actions::Action* a = actions.registry().find(id);
        if (!a) continue;
        auto& b = bar->emplace<ui::Button>("tool." + id, shortName(*a),
                                           [&actions, id] { (void)actions.run(id); });
        b.layout.height = ui::SizeRule::fixed(26);
    }
    return bar;
}

}  // namespace supple::gui
