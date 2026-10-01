// The shell: the arrange view in the window with a transport bar above and a
// status line below. It also owns the wiring between the views and the app:
// selection (UiState), commands (the bus, Origin::Ui) and the meter feed.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app_actions.h"
#include "meter_feed.h"
#include "supple/engine/app.h"
#include "supple/json/value.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/geometry.h"
#include "supple/ui/node.h"
#include "supple/views/arrange.h"

namespace supple::ui {
class Label;
class Root;
}  // namespace supple::ui

namespace supple::gui {

class TransportBar;

class Shell {
  public:
    // The device's record inputs (the shape of `engine.inputs`: running, audio
    // [{channel, port}], midi [names]). The app passes the engine's; a test
    // passes a stub. Empty: the shell asks the bus.
    using InputsSource = std::function<json::Value()>;

    Shell(engine::EngineApp& app, cmd::Dispatcher& dispatcher, UiState& ui, std::string status,
          peaks::PeakService& peaks, MeterFeed& meters, InputsSource inputs = {});
    ~Shell();
    // The tree to hand to ui::Root::setContent (the shell keeps pointers into
    // it). Needs the root (the transport bar's forms are popups) and the
    // actions (its buttons run the same actions the keymap does).
    std::unique_ptr<ui::Node> build(ui::Root& root, AppActions& actions);
    // The playhead lives on the root's overlay tree (§4.3): call after
    // setContent.
    void installOverlay(ui::Root& root);
    // Brings the view up to the model's head version; invalidates what changed.
    void sync();
    views::ArrangeView* arrange() const { return arrange_; }
    // The meters the TCP shows; the app sets the source (EngineApp::meters())
    // and polls once per frame. Shared with the mixer: one feed, one poll.
    MeterFeed& meters() { return meters_; }
    UiState& selection() { return ui_; }
    // The engine's record inputs, refreshed at most twice a second (force asks
    // now — the picker does, so it lists what the device has right now).
    const json::Value& inputs(bool force = false);
    // Shown by the app's status line when a command fails.
    std::function<void(const std::string&)> onError;

  private:
    bool isSelected(const model::Id& track) const;
    void selectTrack(const model::Id& track, unsigned mods);
    void commit(std::string_view command, json::Value args, std::string gesture);
    // What the strip's input button says: the port names when the engine's list
    // is at hand, the channel numbers otherwise.
    std::string inputText(const model::Id& track, const std::string& channels) const;
    void openInputMenu(const model::Id& track, const ui::Rect& anchor);
    // P5.14: the strip's automation lanes (show/hide; created when missing).
    void openEnvelopeMenu(const model::Id& track, const ui::Rect& anchor);

    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    UiState& ui_;
    std::string status_;
    std::uint64_t shownVersion_ = ~std::uint64_t{0};
    ui::Label* counts_ = nullptr;
    peaks::PeakService& peaks_;
    views::ArrangeView* arrange_ = nullptr;
    std::unique_ptr<TransportBar> transport_;
    MeterFeed& meters_;
    InputsSource inputsSource_;
    json::Value inputs_;
    double inputsAtMs_ = -1e9;
    ui::Root* root_ = nullptr;
    std::optional<std::string> selectAnchor_;  // Shift+click extends from here
};

}  // namespace supple::gui
