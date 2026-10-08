// Preferences (Edit > Preferences…): app-wide settings, never project state.
// The monitoring mode, audio thread limit and opt-in process controls are saved to
// <config>/<slug>/preferences.json (supple.preferences/1.2); the wheel zoom
// anchor lives in the mouse map (mouse.json) and is offered here too, with a
// way into the full mouse modifiers dialog.
#pragma once

#include <array>
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
    unsigned audioThreads = 0;  // Automatic; otherwise limit 1..64, including each executor
    bool lockMemory = false;
    bool preventDeepIdle = false;
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
    Status setMemoryLock(bool requested);
    Status setPreventDeepIdle(bool requested);
    void refresh();  // open dialog only; also called by the existing GUI tick
    Status setAudioThreads(unsigned limit);  // saved; next engine start
    void setZoomAnchor(const std::string& behaviour);
    const Preferences& preferences() const { return prefs_; }
    std::function<void(const std::string&)> onError;

  private:
    ui::Root& root_;
    engine::EngineApp& app_;
    MouseDialog& mouse_;
    views::MouseMap& map_;
    Preferences prefs_;
    std::filesystem::path file_;
    ui::Button* monitor_ = nullptr;
    ui::Button* zoom_ = nullptr;
    ui::Button* threads_ = nullptr;
    ui::Button* memory_ = nullptr;
    std::array<ui::Label*, 4> memoryNote_{};
    ui::Button* latency_ = nullptr;
    std::array<ui::Label*, 4> latencyNote_{};
};

}  // namespace supple::gui
