#include "supple/ui/controls.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "supple/ui/root.h"

namespace supple::ui {
namespace {

constexpr double kTopDb = 12.0, kFloorDb = -60.0, kSilence = 0.02;
constexpr float kMeterTop = 6.0f, kMeterFloor = -60.0f;

std::string dbText(double db) {
    if (db <= kFaderSilenceDb + 1e-9) return "-inf";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%+.1f", db);
    return buf;
}

float toDb(float amplitude) { return amplitude > 1e-8f ? 20.0f * std::log10(amplitude) : -150.0f; }

float meterPos(float db) {
    return std::clamp((db - kMeterFloor) / (kMeterTop - kMeterFloor), 0.0f, 1.0f);
}

}  // namespace

double faderPosition(double db) {
    if (db <= kFloorDb) return db <= kFaderSilenceDb + 1e-9 ? 0.0 : kSilence;
    const double x = (std::min(db, kTopDb) - kFloorDb) / (kTopDb - kFloorDb);  // 0..1
    return kSilence + (1.0 - kSilence) * x * x;
}

double faderDb(double position) {
    if (position <= kSilence * 0.5) return kFaderSilenceDb;
    if (position <= kSilence) return kFloorDb;
    const double x = std::sqrt((std::min(position, 1.0) - kSilence) / (1.0 - kSilence));
    return kFloorDb + x * (kTopDb - kFloorDb);
}

// ----------------------------------------------------------------- Fader

Fader::Fader(std::string id, Orientation orientation)
    : Node(std::move(id), Role::Slider), orientation_(orientation) {
    focusable = true;
    interactive = true;
    if (orientation_ == Orientation::Vertical) {
        layout.width = SizeRule::fixed(28);
        layout.height = SizeRule::fraction();
    } else {
        layout.width = SizeRule::fraction();
        layout.height = SizeRule::fixed(18);
    }
}

void Fader::setValue(double db) {
    if (dragging_ || db == db_) return;
    db_ = db;
    invalidate();
}

std::string Fader::valueText() const { return dbText(db_) + " dB"; }

Size Fader::measure(const Canvas&, Size available) {
    return orientation_ == Orientation::Vertical ? Size{28, available.h} : Size{available.w, 18};
}

Rect Fader::track() const {
    const Rect b = bounds();
    return orientation_ == Orientation::Vertical
               ? Rect{b.x + b.w / 2 - 2, b.y + 8, 4, std::max(0.0f, b.h - 16)}
               : Rect{b.x + 6, b.y + b.h / 2 - 2, std::max(0.0f, b.w - 12), 4};
}

void Fader::change(double db, Gesture g) {
    db = std::clamp(db, kFaderSilenceDb, kTopDb);
    if (db != db_ || g == Gesture::Begin || g == Gesture::End) {
        db_ = db;
        invalidate();
        auto callback = onChange;
        if (callback) callback(db_, g);
    }
}

void Fader::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect tr = track();
    c.fillRound(tr, 2, t.color("ground"));
    const auto pos = static_cast<float>(faderPosition(db_));
    const float unity = static_cast<float>(faderPosition(0));
    if (orientation_ == Orientation::Vertical) {
        // Scale marks: +6, 0, -6, -12, -24, -48.
        for (double mark : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
            const float y = tr.bottom() - static_cast<float>(faderPosition(mark)) * tr.h;
            c.fillRect({tr.x - 6, y, mark == 0 ? 16.0f : 4.0f, 1},
                       t.color(mark == 0 ? "ink-quiet" : "rule"));
        }
        const float y = tr.bottom() - pos * tr.h;
        c.fillRect({tr.x, y, tr.w, tr.bottom() - y}, t.color("accent").withAlpha(0.55f));
        const Rect cap{bounds().x + 2, y - 7, bounds().w - 4, 14};
        c.fillRound(cap, 3, dragging_ ? t.color("focus") : t.color("raised"));
        c.strokeRound(cap, 3, focused() ? t.color("focus") : t.color("rule"));
        c.fillRect({cap.x + 3, y - 0.5f, cap.w - 6, 1}, t.color("ink"));
        (void)unity;
    } else {
        const float x = tr.x + pos * tr.w;
        c.fillRect({tr.x + unity * tr.w, tr.y - 3, 1, tr.h + 6}, t.color("ink-quiet"));
        c.fillRect({tr.x, tr.y, x - tr.x, tr.h}, t.color("accent").withAlpha(0.55f));
        const Rect cap{x - 5, bounds().y + 2, 10, bounds().h - 4};
        c.fillRound(cap, 3, dragging_ ? t.color("focus") : t.color("raised"));
        c.strokeRound(cap, 3, focused() ? t.color("focus") : t.color("rule"));
    }
}

bool Fader::onPointer(const PointerEvent& e) {
    using T = PointerEvent::Type;
    switch (e.type) {
        case T::Down:
            if (e.button != 1) return false;
            if (Root* r = root()) r->focus(this);
            if (e.clicks >= 2) {  // reset to unity
                change(0.0, Gesture::Set);
                return true;
            }
            dragging_ = true;
            press_ = e.pos;
            pressDb_ = db_;
            if (Root* r = root()) r->capture(this);
            change(db_, Gesture::Begin);
            return true;
        case T::Move: {
            if (!dragging_) return false;
            // Relative drags: grabbing the cap never jumps; Shift is fine.
            const Rect tr = track();
            const float delta = orientation_ == Orientation::Vertical
                                    ? (press_.y - e.pos.y) / std::max(1.0f, tr.h)
                                    : (e.pos.x - press_.x) / std::max(1.0f, tr.w);
            const double scale = (e.mods & kShift) ? 0.1 : 1.0;
            const double pos =
                std::clamp(faderPosition(pressDb_) + static_cast<double>(delta) * scale, 0.0, 1.0);
            change(faderDb(pos), Gesture::Move);
            return true;
        }
        case T::Cancel:
        case T::Up:
            if (!dragging_) return false;
            dragging_ = false;
            if (Root* r = root()) r->capture(nullptr);
            change(db_, Gesture::End);
            return true;
        case T::Wheel: {
            if (!wheel) return false;  // the wheel scrolls what holds it
            const double step = (e.mods & kShift) ? 0.1 : 0.5;
            const double base = db_ <= kFloorDb ? kFloorDb : db_;
            change(base + static_cast<double>(e.wheelY) * step, Gesture::Set);
            return true;
        }
        default:
            return false;
    }
}

bool Fader::onKey(const KeyEvent& e) {
    const double step = (e.mods & kShift) ? 0.1 : 1.0;
    switch (e.key) {
        case SDLK_UP:
        case SDLK_RIGHT:
            change(std::max(db_, kFloorDb) + step, Gesture::Set);
            return true;
        case SDLK_DOWN:
        case SDLK_LEFT:
            change(db_ - step <= kFloorDb ? kFaderSilenceDb : db_ - step, Gesture::Set);
            return true;
        case SDLK_0:
        case SDLK_HOME:
            change(0.0, Gesture::Set);
            return true;
        default:
            return false;
    }
}

// ----------------------------------------------------------------- PanControl

PanControl::PanControl(std::string id) : Node(std::move(id), Role::Slider) {
    focusable = true;
    interactive = true;
    layout.width = SizeRule::fraction();
    layout.height = SizeRule::fixed(16);
}

void PanControl::setValue(double pan) {
    if (dragging_ || pan == pan_) return;
    pan_ = pan;
    invalidate();
}

std::string PanControl::valueText() const {
    if (std::abs(pan_) < 0.5) return "C";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%s", std::abs(pan_), pan_ < 0 ? "L" : "R");
    return buf;
}

Size PanControl::measure(const Canvas&, Size available) { return {available.w, 16}; }

void PanControl::change(double pan, Gesture g) {
    pan = std::clamp(std::round(pan * 10) / 10, -100.0, 100.0);
    if (pan != pan_ || g == Gesture::Begin || g == Gesture::End) {
        pan_ = pan;
        invalidate();
        auto callback = onChange;
        if (callback) callback(pan_, g);
    }
}

void PanControl::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    const Rect tr{b.x + 2, b.y + 3, b.w - 4, b.h - 6};
    c.fillRound(tr, 3, t.color("ground"));
    const float mid = tr.x + tr.w / 2;
    const float x = mid + static_cast<float>(pan_ / 100.0) * (tr.w / 2);
    c.fillRect({std::min(mid, x), tr.y, std::abs(x - mid), tr.h},
               t.color("accent").withAlpha(0.7f));
    c.fillRect({mid, tr.y - 2, 1, tr.h + 4}, t.color("ink-quiet"));
    c.fillRect({x - 1, tr.y - 1, 2, tr.h + 2},
               dragging_ || focused() ? t.color("focus") : t.color("ink"));
}

bool PanControl::onPointer(const PointerEvent& e) {
    using T = PointerEvent::Type;
    switch (e.type) {
        case T::Down:
            if (e.button != 1) return false;
            if (Root* r = root()) r->focus(this);
            if (e.clicks >= 2) {
                change(0.0, Gesture::Set);
                return true;
            }
            dragging_ = true;
            press_ = e.pos;
            pressPan_ = pan_;
            if (Root* r = root()) r->capture(this);
            change(pan_, Gesture::Begin);
            return true;
        case T::Move: {
            if (!dragging_) return false;
            const double scale = (e.mods & kShift) ? 0.1 : 1.0;
            change(pressPan_ +
                       static_cast<double>((e.pos.x - press_.x) / std::max(1.0f, bounds().w - 4)) *
                           200 * scale,
                   Gesture::Move);
            return true;
        }
        case T::Cancel:
        case T::Up:
            if (!dragging_) return false;
            dragging_ = false;
            if (Root* r = root()) r->capture(nullptr);
            change(pan_, Gesture::End);
            return true;
        case T::Wheel:
            if (!wheel) return false;  // the wheel scrolls what holds it
            change(pan_ + static_cast<double>(e.wheelY) * ((e.mods & kShift) ? 1 : 5),
                   Gesture::Set);
            return true;
        default:
            return false;
    }
}

bool PanControl::onKey(const KeyEvent& e) {
    const double step = (e.mods & kShift) ? 1 : 5;
    switch (e.key) {
        case SDLK_LEFT:
            change(pan_ - step, Gesture::Set);
            return true;
        case SDLK_RIGHT:
            change(pan_ + step, Gesture::Set);
            return true;
        case SDLK_0:
        case SDLK_HOME:
            change(0, Gesture::Set);
            return true;
        default:
            return false;
    }
}

// ------------------------------------------------------------------------- Slider

Slider::Slider(std::string id) : Node(std::move(id), Role::Slider) {
    focusable = true;
    interactive = true;
    layout.width = SizeRule::fraction();
    layout.height = SizeRule::fixed(20);
}

void Slider::setValue(double value) {
    const double v = std::clamp(value, 0.0, 1.0);
    if (v == value_) return;
    value_ = v;
    invalidate();
}

Size Slider::measure(const Canvas&, Size available) { return {available.w, 20}; }

Rect Slider::track() const {
    const Rect b = bounds();
    return {b.x + kInset, b.y, std::max(1.0f, b.w - 2 * kInset), b.h};
}

void Slider::change(double value, Gesture g) {
    const double v = std::clamp(value, 0.0, 1.0);
    if (v != value_ || g == Gesture::Begin || g == Gesture::End) {
        value_ = v;
        invalidate();
        auto callback = onChange;
        if (callback) callback(value_, g);
    }
}

void Slider::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    const float y = b.y + b.h / 2;
    c.line({b.x + kInset, y}, {b.right() - kInset, y}, t.color("rule"), 3.0f);
    const float hx = b.x + kInset + static_cast<float>(value_) * (b.w - 2 * kInset);
    c.line({b.x + kInset, y}, {hx, y}, t.color("accent"), 3.0f);
    const Rect cap{hx - 4, b.y + 2, 8, b.h - 4};
    c.fillRound(cap, 3, dragging_ ? t.color("focus") : t.color("raised"));
    c.strokeRound(cap, 3, focused() ? t.color("focus") : t.color("rule"));
}

bool Slider::onPointer(const PointerEvent& e) {
    using T = PointerEvent::Type;
    switch (e.type) {
        case T::Down:
            if (e.button != 1) return false;
            if (Root* r = root()) r->focus(this);
            if (e.clicks >= 2) {  // reset to the caller's default
                change(defaultValue_, Gesture::Set);
                return true;
            }
            dragging_ = true;
            press_ = e.pos;
            pressValue_ = value_;
            if (Root* r = root()) r->capture(this);
            change(value_, Gesture::Begin);  // a press never jumps the value
            return true;
        case T::Move: {
            if (!dragging_) return false;
            const double scale = (e.mods & kShift) ? 0.1 : 1.0;
            const double width = static_cast<double>(std::max(1.0f, track().w));
            const double delta = static_cast<double>(e.pos.x - press_.x) / width;
            change(pressValue_ + delta * scale, Gesture::Move);
            return true;
        }
        case T::Cancel:
        case T::Up:
            if (!dragging_) return false;
            dragging_ = false;
            if (Root* r = root()) r->capture(nullptr);
            change(value_, Gesture::End);
            return true;
        case T::Wheel:
            if (!wheel) return false;  // the wheel scrolls what holds it
            change(value_ + static_cast<double>(e.wheelY) * ((e.mods & kShift) ? 0.005 : 0.02),
                   Gesture::Set);
            return true;
        default:
            return false;
    }
}

bool Slider::onKey(const KeyEvent& e) {
    const double step = (e.mods & kShift) ? 0.001 : 0.01;
    switch (e.key) {
        case SDLK_RIGHT:
        case SDLK_UP:
            change(value_ + step, Gesture::Set);
            return true;
        case SDLK_LEFT:
        case SDLK_DOWN:
            change(value_ - step, Gesture::Set);
            return true;
        case SDLK_HOME:
            change(0.0, Gesture::Set);
            return true;
        case SDLK_END:
            change(1.0, Gesture::Set);
            return true;
        default:
            return false;
    }
}

std::string Slider::valueText() const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.4f", value_);
    return buf;
}

// ----------------------------------------------------------------- Meter

Meter::Meter(std::string id, bool vertical) : Node(std::move(id)), vertical_(vertical) {
    interactive = true;
    if (vertical_) {
        layout.width = SizeRule::fixed(12);
        layout.height = SizeRule::fraction();
    } else {
        layout.width = SizeRule::fraction();
        layout.height = SizeRule::fixed(8);
    }
}

void Meter::reset() {
    for (int ch = 0; ch < 2; ++ch) {
        peakDb_[ch] = rmsDb_[ch] = holdDb_[ch] = -150;
        clip_[ch] = false;
    }
    invalidate();
}

void Meter::push(const float peak[2], const float rms[2]) {
    const auto now = std::chrono::steady_clock::now();
    tick();
    bool moved = false;
    for (int ch = 0; ch < 2; ++ch) {
        const float p = toDb(peak[ch]), r = toDb(rms[ch]);
        if (p > peakDb_[ch]) {
            peakDb_[ch] = p;
            moved = true;
        }
        if (r > rmsDb_[ch]) {
            rmsDb_[ch] = r;
            moved = true;
        }
        if (p >= holdDb_[ch]) {
            holdDb_[ch] = p;
            holdAt_[ch] = now;
        }
        if (peak[ch] >= 1.0f) clip_[ch] = true;
    }
    if (moved) invalidate();
}

void Meter::tick() {
    const auto now = std::chrono::steady_clock::now();
    if (last_.time_since_epoch().count() == 0) last_ = now;
    const float dt = std::chrono::duration<float>(now - last_).count();
    last_ = now;
    bool moving = false;
    for (int ch = 0; ch < 2; ++ch) {
        // Peaks fall at 26 dB/s, RMS at 20 dB/s; holds stay 1.5 s, then fall.
        // A meter already at the floor is at rest: nothing to draw and no
        // animation frame to ask for (an idle meter must not cost anything).
        auto fall = [&](float& value, float dbPerSecond) {
            if (value <= kMeterFloor) return;
            const float next = std::max(static_cast<float>(kMeterFloor), value - dbPerSecond * dt);
            if (next == value) return;
            value = next;
            moving = true;
        };
        fall(peakDb_[ch], 26.0f);
        fall(rmsDb_[ch], 20.0f);
        if (now - holdAt_[ch] > std::chrono::milliseconds(1500)) fall(holdDb_[ch], 40.0f);
    }
    if (moving) {
        invalidate();
        if (Root* r = root()) r->requestAnimationFrame();
    }
}

void Meter::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    c.fillRect(b, t.color("ground"));
    const float gap = 1;
    for (int ch = 0; ch < 2; ++ch) {
        Rect lane =
            vertical_
                ? Rect{b.x + static_cast<float>(ch) * (b.w / 2), b.y + 4, b.w / 2 - gap, b.h - 4}
                : Rect{b.x + 4, b.y + static_cast<float>(ch) * (b.h / 2), b.w - 4, b.h / 2 - gap};
        auto fill = [&](float db, Color col) {
            const float p = meterPos(db);
            if (p <= 0) return;
            if (vertical_)
                c.fillRect({lane.x, lane.bottom() - p * lane.h, lane.w, p * lane.h}, col);
            else
                c.fillRect({lane.x, lane.y, p * lane.w, lane.h}, col);
        };
        fill(peakDb_[ch],
             peakDb_[ch] > -6 ? t.color("meter-hot") : t.color("meter").withAlpha(0.55f));
        fill(rmsDb_[ch], t.color("meter"));
        if (holdDb_[ch] > kMeterFloor) {
            const float p = meterPos(holdDb_[ch]);
            if (vertical_)
                c.fillRect({lane.x, lane.bottom() - p * lane.h, lane.w, 1.5f},
                           t.color(holdDb_[ch] > 0 ? "clip" : "ink"));
            else
                c.fillRect({lane.x + p * lane.w, lane.y, 1.5f, lane.h},
                           t.color(holdDb_[ch] > 0 ? "clip" : "ink"));
        }
        // Clip indicator at the top (or end).
        const Color clipCol = clip_[ch] ? t.color("clip") : t.color("panel");
        if (vertical_)
            c.fillRect({lane.x, b.y, lane.w, 3}, clipCol);
        else
            c.fillRect({b.x, lane.y, 3, lane.h}, clipCol);
    }
    // 0 dB mark.
    const float z = meterPos(0);
    if (vertical_)
        c.fillRect({b.x, b.y + 4 + (1 - z) * (b.h - 4), b.w, 1}, t.color("ink-faint"));
    else
        c.fillRect({b.x + 4 + z * (b.w - 4), b.y, 1, b.h}, t.color("ink-faint"));
}

bool Meter::onPointer(const PointerEvent& e) {
    if (e.type == PointerEvent::Type::Down && e.button == 1) {
        clip_[0] = clip_[1] = false;
        holdDb_[0] = holdDb_[1] = -150;
        invalidate();
        return true;
    }
    return false;
}

// ----------------------------------------------------------------- Toggle

Toggle::Toggle(std::string id, std::string text, std::string onColor,
               std::function<void(bool)> toggle)
    : Node(std::move(id), Role::Button),
      onToggle(std::move(toggle)),
      text_(std::move(text)),
      onColor_(std::move(onColor)) {
    focusable = true;
    interactive = true;
    layout.width = SizeRule::fixed(22);
    layout.height = SizeRule::fixed(20);
}

void Toggle::setOn(bool on) {
    if (on == on_) return;
    on_ = on;
    invalidate();
}

Size Toggle::measure(const Canvas& canvas, Size) {
    return {canvas.textWidth(text_, Font::UiBold, canvas.theme().type("caption")) + 10, 20};
}

void Toggle::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    if (pill) {
        const float r = std::min(b.w, b.h) / 2;
        const Color colour = t.color(onColor_);
        if (on_) {
            c.fillRound(b, r, colour);
        } else {
            c.fillRound(b, r, pressed_ ? t.color("selection") : t.color("ground"));
            if (hovered()) c.fillRound(b, r, colour.withAlpha(0.15f));
            c.strokeRound(b, r, colour.withAlpha(0.55f));
        }
        if (focused()) c.strokeRound(b.inset(-1), r + 1, t.color("focus"));
        c.text(b, text_, Font::UiBold, t.type("caption"),
               on_ ? t.color("ground") : colour.withAlpha(0.9f), kCenter | kMiddle);
        return;
    }
    const Color face = on_         ? t.color(onColor_)
                       : pressed_  ? t.color("selection")
                       : hovered() ? t.color("raised")
                                   : t.color("panel");
    c.fillRound(b, t.radius("s"), face);
    c.strokeRound(b, t.radius("s"), focused() ? t.color("focus") : t.color("rule"));
    c.text(b, text_, Font::UiBold, t.type("caption"),
           on_ ? t.color("ground") : t.color("ink-quiet"), kCenter | kMiddle);
}

bool Toggle::onPointer(const PointerEvent& e) {
    if (e.button != 1 && e.type != PointerEvent::Type::Move && e.type != PointerEvent::Type::Cancel)
        return false;
    switch (e.type) {
        case PointerEvent::Type::Down:
            pressed_ = true;
            invalidate();
            return true;
        case PointerEvent::Type::Cancel:
            pressed_ = false;
            invalidate();
            return true;
        case PointerEvent::Type::Up: {
            const bool fire = pressed_ && bounds().contains(e.pos);
            pressed_ = false;
            invalidate();
            if (fire) {
                on_ = !on_;
                auto callback = onToggle;
                if (callback) callback(on_);
            }
            return true;
        }
        default:
            return false;
    }
}

bool Toggle::onKey(const KeyEvent& e) {
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        on_ = !on_;
        invalidate();
        auto callback = onToggle;
        if (callback) callback(on_);
        return true;
    }
    return false;
}

// ----------------------------------------------------------------- ArmButton

ArmButton::ArmButton(std::string id, std::function<void(bool)> toggle)
    : Toggle(std::move(id), "", "armed", std::move(toggle)) {
    layout.width = SizeRule::fixed(20);
    layout.height = SizeRule::fixed(20);
}

void ArmButton::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    const Point centre{b.x + b.w / 2, b.y + b.h / 2};
    const float r = std::max(2.0f, std::min(b.w, b.h) / 2 - 1);
    const Color red = t.color("armed");
    if (on()) {
        c.fillCircle(centre, r, red);
        c.fillCircle({centre.x - r * 0.3f, centre.y - r * 0.3f}, r * 0.3f,
                     Color{1, 1, 1, 0.25f});  // a little shine
    } else {
        c.fillCircle(centre, r, red.withAlpha(pressed() ? 0.5f : hovered() ? 0.4f : 0.28f));
        c.strokeCircle(centre, r, red.withAlpha(0.6f));
    }
    if (focused()) c.strokeCircle(centre, r + 2, t.color("focus"));
}

// ----------------------------------------------------------------- MonitorButton

MonitorButton::MonitorButton(std::string id, std::function<void()> click)
    : Node(std::move(id), Role::Button), onClick(std::move(click)) {
    focusable = true;
    interactive = true;
    layout.width = SizeRule::fixed(20);
    layout.height = SizeRule::fixed(20);
}

void MonitorButton::setMode(Mode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    invalidate();
}

void MonitorButton::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    if (pressed_ || hovered())
        c.fillRound(b, t.radius("s"), pressed_ ? t.color("selection") : t.color("raised"));
    if (focused()) c.strokeRound(b, t.radius("s"), t.color("focus"));
    // The speaker in a square that fits the button, left of its centre so the
    // waves have room.
    const float s = std::min(b.w, b.h) - 4;
    const float x = b.x + (b.w - s) / 2, y = b.y + (b.h - s) / 2;
    const std::vector<Point> speaker = {
        {x + s * 0.08f, y + s * 0.36f}, {x + s * 0.28f, y + s * 0.36f},
        {x + s * 0.54f, y + s * 0.12f}, {x + s * 0.54f, y + s * 0.88f},
        {x + s * 0.28f, y + s * 0.64f}, {x + s * 0.08f, y + s * 0.64f}};
    const Color ink = mode_ == Mode::Auto ? t.color("meter")
                      : mode_ == Mode::On ? t.color("ink")
                                          : t.color("ink-quiet");
    if (mode_ == Mode::Off) {
        c.strokePath(speaker, ink, 1.2f);
        return;
    }
    c.fillPath(speaker, ink);
    const Point centre{x + s * 0.54f, y + s * 0.5f};
    c.strokeArc(centre, s * 0.2f, -0.9f, 0.9f, ink, 1.4f);
    c.strokeArc(centre, s * 0.38f, -0.9f, 0.9f, ink, 1.4f);
}

bool MonitorButton::onPointer(const PointerEvent& e) {
    if (e.button != 1 && e.type != PointerEvent::Type::Move && e.type != PointerEvent::Type::Cancel)
        return false;
    switch (e.type) {
        case PointerEvent::Type::Down:
            pressed_ = true;
            invalidate();
            return true;
        case PointerEvent::Type::Cancel:
            pressed_ = false;
            invalidate();
            return true;
        case PointerEvent::Type::Up: {
            const bool fire = pressed_ && bounds().contains(e.pos);
            pressed_ = false;
            invalidate();
            auto callback = onClick;
            if (fire && callback) callback();
            return true;
        }
        default:
            return false;
    }
}

bool MonitorButton::onKey(const KeyEvent& e) {
    if (e.key != SDLK_RETURN && e.key != SDLK_KP_ENTER) return false;
    if (auto callback = onClick) callback();
    return true;
}

}  // namespace supple::ui
