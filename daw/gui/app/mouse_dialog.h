// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The mouse modifiers dialog (gap log #37): per context of the arrange view,
// what each modifier combination does. Edits apply at once and are saved to
// <config>/<slug>/mouse.json.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "supple/ui/root.h"
#include "supple/ui/widgets.h"
#include "supple/views/mouse_map.h"

namespace supple::gui {

class MouseDialog {
  public:
    // `map` is the live one (the arrange view's); `file` empty: not saved.
    MouseDialog(ui::Root& root, views::MouseMap& map, std::filesystem::path file)
        : root_(root), map_(map), file_(std::move(file)) {}
    void open(std::string context = "item.drag");
    // Sets one combination from the dialog's menus (and the tests'): an empty
    // behaviour makes it the plain press.
    void set(const std::string& context, unsigned mods, const std::string& behaviour);
    void resetContext(const std::string& context);
    std::function<void(const std::string&)> onError;

  private:
    void save();
    void refresh();  // the open dialog's buttons from the map
    ui::Root& root_;
    views::MouseMap& map_;
    std::filesystem::path file_;
    std::string context_;
    std::vector<std::pair<unsigned, ui::Button*>> buttons_;  // the open dialog's rows
};

}  // namespace supple::gui
