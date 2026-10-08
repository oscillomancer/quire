// The actions list (gui.md §6, "?"): fuzzy search over every action, shows
// bound keys and MIDI controls, runs (asking for prompted arguments), binds
// keys (Ctrl+B) and learns a MIDI control (Ctrl+L); with the chrome, puts an
// action on the toolbar (Ctrl+T) or in a menu (Ctrl+M); Reset keys…
// restores the default bindings.
#pragma once

#include <functional>

#include "app_actions.h"
#include "supple/ui/root.h"

namespace supple::gui {

class Chrome;

// Ctrl+B, Ctrl+L, Ctrl+T and Ctrl+M work with the focus on the search field or the
// list; a new binding replaces an arrange-view default on the same keys.
void openActionsList(ui::Root& root, AppActions& actions, Chrome* chrome = nullptr);
// Asks first, then puts every key binding back to the defaults (saved);
// `done` runs after a reset.
void openKeymapReset(ui::Root& root, AppActions& actions, std::function<void()> done = {});
// A form asking for an action's typed arguments, then running it (the actions
// list, menus, the toolbar and keys share it through AppActions::onPrompt).
void openActionPrompt(ui::Root& root, AppActions& actions, const actions::Action& action);

}  // namespace supple::gui
