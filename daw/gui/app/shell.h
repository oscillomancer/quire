// The shell: the arrange view in the window with a transport line above and a
// status line below. It proves the window ↔ model ↔ toolkit wiring; the other
// views (mixer, editors) follow (docs/specs/gui.md §11).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "supple/engine/app.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/node.h"
#include "supple/views/arrange.h"

namespace supple::ui {
class Label;
class Root;
}  // namespace supple::ui

namespace supple::gui {

class Shell {
  public:
    Shell(engine::EngineApp& app, std::string status, peaks::PeakService& peaks);
    // The tree to hand to ui::Root::setContent (the shell keeps pointers into it).
    std::unique_ptr<ui::Node> build();
    // The playhead lives on the root's overlay tree (§4.3): call after
    // setContent.
    void installOverlay(ui::Root& root);
    // Brings the view up to the model's head version; invalidates what changed.
    void sync();
    views::ArrangeView* arrange() const { return arrange_; }

  private:
    engine::EngineApp& app_;
    std::string status_;
    std::uint64_t shownVersion_ = ~std::uint64_t{0};
    ui::Label* counts_ = nullptr;
    peaks::PeakService& peaks_;
    views::ArrangeView* arrange_ = nullptr;
};

}  // namespace supple::gui
