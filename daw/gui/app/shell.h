// The P5.1 shell: a read-only view of the session (transport line and the
// strip tree with type badges). It proves the window ↔ model wiring; the
// toolkit and the real views replace it (docs/specs/gui.md §11).
#pragma once

#include <cstdint>
#include <string>

#include "supple/engine/app.h"
#include "supple/gui/window.h"

namespace supple::gui {

class Shell {
  public:
    Shell(const engine::EngineApp& app, Fonts fonts, std::string status)
        : app_(app), fonts_(fonts), status_(std::move(status)) {}
    // True when the model has moved since the last draw().
    bool stale() const { return app_.head().version != drawnVersion_; }
    void draw(NVGcontext* vg, float width, float height);
    static constexpr std::uint32_t kGround = 0x1c1a17;

  private:
    const engine::EngineApp& app_;
    Fonts fonts_;
    std::string status_;
    std::uint64_t drawnVersion_ = ~std::uint64_t{0};
};

}  // namespace supple::gui
