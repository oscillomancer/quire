// The app's actions: every command, the built-ins the default keymap names,
// the user's keymap, and key routing (gui.md §6). UI state the actions read
// (selection, playhead) comes from UiState.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "supple/actions/actions.h"
#include "supple/engine/app.h"

namespace supple::gui {

// Per-project UI state (gui.md §1.2); persisted later to <project>.ui.json.
struct UiState {
    std::vector<std::string> selectedTracks, selectedItems;
};

// GUI-only commands: `gui.selection` (read-only selection for agents, §1.2).
void registerGuiCommands(cmd::Registry& registry, const UiState& ui);

class AppActions {
  public:
    AppActions(const cmd::Registry& registry, cmd::Dispatcher& dispatcher, engine::EngineApp& app,
               UiState& ui, std::filesystem::path keymapFile);

    actions::ActionRegistry& registry() { return registry_; }
    actions::Keymap& keymap() { return keymap_; }
    const std::filesystem::path& keymapFile() const { return keymapFile_; }
    actions::Context context(std::vector<std::string> sections = {"arrange", "global"}) const;
    Result<json::Value> run(const actions::Action& action,
                            const json::Value& prompted = json::Value::object());
    Result<json::Value> run(std::string_view id);
    // A key the focused widget did not use: true when it ran (or started) a
    // binding. `typing` (a text field has focus) lets only modified chords through.
    bool key(int keycode, unsigned mods, bool typing);
    // Built-in UI actions the app provides (open the actions list, …).
    void addUi(std::string id, std::string name, std::function<void()> run);
    // A built-in with one prompted argument (the actions list asks for it).
    void addUi(std::string id, std::string name, std::string prompt,
               std::function<Status(const json::Value& value)> run);
    // Shown by the app (status line): the last action's error, if any.
    std::function<void(const std::string&)> onError;
    std::string keysFor(std::string_view action) const;  // "Ctrl+Z", or ""

  private:
    actions::ActionRegistry registry_;
    actions::Keymap keymap_;
    std::filesystem::path keymapFile_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    actions::Runner runner_;
};

}  // namespace supple::gui
