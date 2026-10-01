// The actions list (gui.md §6, "?"): fuzzy search over every action, shows
// bound keys, runs (asking for prompted arguments), binds keys.
#pragma once

#include "app_actions.h"
#include "supple/ui/root.h"

namespace supple::gui {

void openActionsList(ui::Root& root, AppActions& actions);

}  // namespace supple::gui
