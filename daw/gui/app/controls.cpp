#include "controls.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cmath>

namespace supple::gui {

Slider::Slider(std::string id, std::function<void(double)> onSet, std::function<void()> onBegin,
               std::function<void()> onEnd)
    : Node(std::move(id), ui::Role::Slider),
      onSet_(std::move(onSet)),
      onBegin_(std::move(onBegin)),
      onEnd_(std::move(onEnd)) {
    interactive = true;
    focusable = true;
    layout.height = ui::SizeRule::fixed(20);
    layout.width = ui::SizeRule::fraction();
    layout.align = ui::Layout::Align::Center;
}

void Slider::setValue(double normalized) {
    const double v = std::clamp(normalized, 0.0, 1.0);
    if (v == value_) return;
    value_ = v;
    invalidate();
}

double Slider::normalizedAt(float x) const {
    const ui::Rect b = bounds();
    const float track = std::max(1.0f, b.w - 2 * kInset);
    return std::clamp(static_cast<double>((x - b.x - kInset) / track), 0.0, 1.0);
}

void Slider::emit(ui::Point p) {
    const double v = normalizedAt(p.x);
    setValue(v);
    if (onSet_) onSet_(value_);
}

void Slider::paint(ui::Canvas& c) {
    const ui::Theme& t = c.theme();
    const ui::Rect b = bounds();
    const float inset = kInset;
    const float y = b.y + b.h / 2;
    c.line({b.x + inset, y}, {b.right() - inset, y}, t.color("rule"), 3.0f);
    const float hx = b.x + inset + static_cast<float>(value_) * (b.w - 2 * inset);
    c.line({b.x + inset, y}, {hx, y}, t.color("accent"), 3.0f);
    const ui::Color knob = focused() ? t.color("focus") : t.color("ink");
    c.fillRound({hx - 4, b.y + 2, 8, b.h - 4}, t.radius("s"), knob);
}

bool Slider::onPointer(const ui::PointerEvent& e) {
    switch (e.type) {
        case ui::PointerEvent::Type::Down:
            if (e.button != 1) return false;
            dragging_ = true;
            if (onBegin_) onBegin_();
            emit(e.pos);
            return true;
        case ui::PointerEvent::Type::Move:
            if (!dragging_) return false;
            emit(e.pos);
            return true;
        case ui::PointerEvent::Type::Up:
            if (!dragging_) return false;
            dragging_ = false;
            emit(e.pos);
            if (onEnd_) onEnd_();
            return true;
        case ui::PointerEvent::Type::Wheel:
            setValue(value_ + (e.wheelY > 0 ? 0.02 : -0.02));
            if (onSet_) onSet_(value_);
            return true;
        default:
            return false;
    }
}

bool Slider::onKey(const ui::KeyEvent& e) {
    const double step = (e.mods & ui::kShift) ? 0.001 : 0.01;
    switch (e.key) {
        case SDLK_LEFT:
        case SDLK_DOWN:
            setValue(value_ - step);
            break;
        case SDLK_RIGHT:
        case SDLK_UP:
            setValue(value_ + step);
            break;
        default:
            return false;
    }
    if (onSet_) onSet_(value_);
    return true;
}

std::string Slider::valueText() const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.4f", value_);
    return buf;
}

}  // namespace supple::gui
