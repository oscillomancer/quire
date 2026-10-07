// The actions list (gui.md §6, "?"): fuzzy search over every action, shows
// bound keys, runs (asking for prompted arguments), binds keys; with the
// chrome, puts an action on the toolbar (Ctrl+T) or in a menu (Ctrl+M).
#pragma once

#include "app_actions.h"
#include "supple/ui/root.h"

namespace supple::gui {

class Chrome;

void openActionsList(ui::Root& root, AppActions& actions, Chrome* chrome = nullptr);
// A form asking for an action's typed arguments, then running it (the actions
// list, menus, the toolbar and keys share it through AppActions::onPrompt).
void openActionPrompt(ui::Root& root, AppActions& actions, const actions::Action& action);

}  // namespace supple::gui
