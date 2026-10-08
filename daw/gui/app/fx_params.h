// The semantic parameter panel (docs/specs/gui.md §4.5): a tab in the FX
// window, generated from the plugin's semantic schema through `fx.describe`
// (the same names, units, ranges and groups an agent reads). Edits go through
// `fx.param.set`; a slider drag is one undo step (Origin::gesture).
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>

#include "learn_dialog.h"
#include "supple/engine/app.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Root;
}

namespace supple::gui {

class ParamPanel {
  public:
    ParamPanel(cmd::Dispatcher& dispatcher, ui::Root& root);

    // Builds the rows for `fx` under `parent` (a column). Returns how many
    // parameters are shown; an error is returned to display in the panel.
    Result<std::size_t> build(ui::Node& parent, model::Id fx);
    // Errors from edits (shown by the window).
    std::function<void(const std::string&)> onError;
    // Opens the MIDI learn dialog (each row's "MIDI" button; unset: no button).
    std::function<void(LearnRequest)> openLearn;

  private:
    // A row's MIDI learn button: the bound control, or "MIDI".
    void learnButton(ui::Node& row, const std::string& id, const std::string& param,
                     const std::string& title);
    void learn(const std::string& param, const std::string& title);
    std::map<std::string, LearnChoice> learned_;  // by parameter, for the shown effect
    void setValue(const std::string& key, json::Value value, bool gesture);
    // Shows the parameter's automation lane, creating the envelope (flat at
    // the current value) when it has none (P5.14).
    Status automate(const std::string& key, double physical);

    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    model::Id fx_;
    std::string gesture_;
    std::uint64_t gestureSerial_ = 0;
};

}  // namespace supple::gui
