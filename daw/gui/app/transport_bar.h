// The transport bar (gui.md §4.1): play/stop, record, loop and punch buttons
// (the same actions the keymap runs), the position in bars|beats and time, the
// session tempo and meter (click to edit them), and the engine's readouts.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "app_actions.h"
#include "supple/engine/app.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Button;
class Label;
class Root;
}  // namespace supple::ui

namespace supple::gui {

class TransportBar {
  public:
    TransportBar(engine::EngineApp& app, AppActions& actions, cmd::Dispatcher& dispatcher,
                 ui::Root& root);
    std::unique_ptr<ui::Node> build();
    // Reads the transport every call and the engine status at most 4 times a
    // second (the readouts must not cost anything while the engine is idle).
    void sync();

  private:
    void openTempoForm();
    void openMeterForm();

    engine::EngineApp& app_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    ui::Button* play_ = nullptr;
    ui::Button* record_ = nullptr;
    ui::Button* loop_ = nullptr;
    ui::Button* punch_ = nullptr;
    ui::Button* click_ = nullptr;  // the metronome
    ui::Button* tempo_ = nullptr;
    ui::Button* meter_ = nullptr;
    ui::Label* position_ = nullptr;
    ui::Label* time_ = nullptr;
    ui::Label* engine_ = nullptr;
    double lastStatusMs_ = -1e9;
};

}  // namespace supple::gui
