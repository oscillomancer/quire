// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// One row of a popup menu: what Ui::popup() shows, and what a host's own
// context-menu items arrive as (EditorHost::hostMenu).
#pragma once

#include <cstdint>
#include <string>

namespace supple::gui {

struct MenuItem {
    enum class Kind : uint8_t { Item, Header, Separator };
    std::string label;
    Kind kind = Kind::Item;
    bool checked = false;
    bool enabled = true;

    static MenuItem item(std::string text, bool checked = false, bool enabled = true) {
        return {std::move(text), Kind::Item, checked, enabled};
    }
    static MenuItem header(std::string text) { return {std::move(text), Kind::Header, false, false}; }
    static MenuItem separator() { return {"", Kind::Separator, false, false}; }
    bool selectable() const { return kind == Kind::Item && enabled; }
};

}  // namespace supple::gui
