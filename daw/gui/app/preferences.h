// Preferences (Edit > Preferences…): app-wide settings, never project state.
// The new-channel monitoring mode and background thread limit are saved to
// <config>/<slug>/preferences.json (supple.preferences/1.1); the wheel zoom
// anchor lives in the mouse map (mouse.json) and is offered here too, with a
// way into the full mouse modifiers dialog.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "mouse_dialog.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::gui {

struct Preferences {
    model::Monitor defaultMonitor = model::Monitor::On;
    unsigned audioThreads = 0;  // Automatic; otherwise limit 1..64, including renderer
    static std::filesystem::path defaultFile(std::string_view slug);
    static Result<Preferences> load(const std::filesystem::path& file);  // absent: defaults
    Status save(const std::filesystem::path& file) const;
};

class PreferencesDialog {
  public:
    // `file` empty: not saved (tests, offscreen runs). The app's default
    // monitoring follows `prefs` from construction on.
    PreferencesDialog(ui::Root& root, engine::EngineApp& app, MouseDialog& mouse,
                      views::MouseMap& map, Preferences prefs, std::filesystem::path file);
    void open();
    bool isOpen() const;
    void setDefaultMonitor(model::Monitor m);  // applied at once and saved
    Status setAudioThreads(unsigned limit);    // saved; next engine start
    void setZoomAnchor(const std::string& behaviour);
    const Preferences& preferences() const { return prefs_; }
    std::function<void(const std::string&)> onError;

  private:
    void refresh();  // the open dialog's buttons
    ui::Root& root_;
    engine::EngineApp& app_;
    MouseDialog& mouse_;
    views::MouseMap& map_;
    Preferences prefs_;
    std::filesystem::path file_;
    ui::Button* monitor_ = nullptr;
    ui::Button* zoom_ = nullptr;
    ui::Button* threads_ = nullptr;
};

}  // namespace supple::gui
