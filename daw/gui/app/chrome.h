// Menus and the toolbar, built from actions (gui.md §6) and customizable in
// $XDG_CONFIG_HOME/<slug>/menus.json (supple.menus/1.0):
//   {"schema": "supple.menus/1.0",
//    "menus": [{"title": "File", "items": ["project.save", "-", "ui.quit"]}],
//    "toolbar": ["transport.toggle", "history.undo"]}
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app_actions.h"
#include "supple/ui/root.h"

namespace supple::gui {

struct ChromeLayout {
    struct Menu {
        std::string title;
        std::vector<std::string> items;  // action ids; "-" is a separator
    };
    std::vector<Menu> menus;
    std::vector<std::string> toolbar;
    static ChromeLayout defaults();
    static Result<ChromeLayout> load(const std::filesystem::path& file);  // defaults when absent
    static std::filesystem::path defaultFile(std::string_view slug);
};

// The menu bar and toolbar as one row node.
std::unique_ptr<ui::Node> makeChrome(ui::Root& root, AppActions& actions, ChromeLayout layout);

}  // namespace supple::gui
