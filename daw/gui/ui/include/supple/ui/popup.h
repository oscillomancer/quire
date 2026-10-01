// Popups on the overlay tree (menus, the actions list, pickers): placed next
// to an anchor and kept inside the window; a click outside or Escape closes
// the topmost one; focus moves into the popup and returns on close.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "supple/ui/node.h"

namespace supple::ui {

enum class Placement { Below, Above, Right, Center };

struct MenuItem {
    std::string label, shortcut;
    std::function<void()> run;
    bool enabled = true, separator = false, checked = false;
};

// A menu's content node (a list); `close` is called before an item runs.
std::unique_ptr<Node> makeMenu(std::string id, std::vector<MenuItem> items,
                               std::function<void()> close);

}  // namespace supple::ui
