// The semantic parameter panel (docs/specs/gui.md §4.5): a tab in the FX
// window, generated from the plugin's semantic schema through `fx.describe`
// (the same names, units, ranges and groups an agent reads). Edits go through
// `fx.param.set`; a slider drag is one undo step (Origin::gesture).
#pragma once

#include <cstddef>
#include <functional>
#include <string>

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

  private:
    void setValue(const std::string& key, json::Value value, bool gesture);

    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    model::Id fx_;
    std::string gesture_;
};

}  // namespace supple::gui
