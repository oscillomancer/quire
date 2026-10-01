// The shell: a read-only view of the session (transport line and the strip
// tree with type badges) built on the toolkit. It proves the window ↔ model ↔
// toolkit wiring; the real views replace it (docs/specs/gui.md §11).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "supple/engine/app.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/node.h"
#include "supple/views/lanes.h"

namespace supple::ui {
class Label;
}

namespace supple::gui {

class Shell {
  public:
    Shell(const engine::EngineApp& app, std::string status, peaks::PeakService& peaks);
    // The tree to hand to ui::Root::setContent (the shell keeps pointers into it).
    std::unique_ptr<ui::Node> build();
    // Brings the view up to the model's head version; invalidates what changed.
    void sync();
    views::Lanes* lanes() const { return lanes_; }

  private:
    const engine::EngineApp& app_;
    std::string status_;
    std::uint64_t shownVersion_ = ~std::uint64_t{0};
    ui::Label* counts_ = nullptr;
    ui::Node* strips_ = nullptr;
    peaks::PeakService& peaks_;
    views::Lanes* lanes_ = nullptr;
};

}  // namespace supple::gui
