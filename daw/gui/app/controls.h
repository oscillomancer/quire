// A horizontal slider for the FX parameter and wet/dry panels. The value is
// normalized [0, 1]; the caller maps it through the parameter's curve. A drag
// reports begin/move/end so the caller can coalesce its commands into one undo
// step (gui.md §1, §4.3).
#pragma once

#include <functional>
#include <string>

#include "supple/ui/node.h"

namespace supple::gui {

class Slider : public ui::Node {
  public:
    Slider(std::string id, std::function<void(double)> onSet, std::function<void()> onBegin = {},
           std::function<void()> onEnd = {});
    void setValue(double normalized);  // 0..1, no callback
    double value() const { return value_; }

    void paint(ui::Canvas& canvas) override;
    bool onPointer(const ui::PointerEvent& e) override;
    bool onKey(const ui::KeyEvent& e) override;
    std::string valueText() const override;

    static constexpr float kInset = 10;  // knob travel inset per side

  private:
    double normalizedAt(float x) const;
    void emit(ui::Point p);

    double value_ = 0;
    bool dragging_ = false;
    std::function<void(double)> onSet_;
    std::function<void()> onBegin_, onEnd_;
};

}  // namespace supple::gui
