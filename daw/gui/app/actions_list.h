// The actions list (gui.md §6, "?"): fuzzy search over every action, shows
// bound keys, runs (asking for prompted arguments), binds keys; with the
// chrome, puts an action on the toolbar (Ctrl+T) or in a menu (Ctrl+M).
#pragma once

#include "app_actions.h"
#include "supple/ui/root.h"

namespace supple::gui {

class Chrome;

void openActionsList(ui::Root& root, AppActions& actions, Chrome* chrome = nullptr);

}  // namespace supple::gui
