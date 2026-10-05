#include "mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL3/SDL_keycode.h>

#include "panel_height.h"
#include "supple/ui/controls.h"
#include "supple/ui/list.h"
#include "supple/ui/widgets.h"
#include "supple/views/track_layout.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
constexpr float kStripW = 92;
// The strip's chain section is the only one that follows the panel's height;
// the rest (name, sends, rec, pan, fader, value, M/S, gaps, padding) is fixed.
constexpr float kSlotH = 18, kSlotGap = 2;
constexpr float kGripH = 5, kHeaderH = 30;
constexpr float kFaderH = 133;       // the fader and meter row
constexpr float kStripChrome = 159;  // the fixed rows, their gaps and the padding
constexpr float kDragThreshold = 4;

// fx.move's index (among the chain without the plug-in) for dropping the
// plug-in at `from` into the gap before slot `gap`; nullopt when it stays.
std::optional<std::size_t> chainMoveIndex(std::size_t from, std::size_t gap) {
    const std::size_t index = gap > from ? gap - 1 : gap;
    if (index == from) return std::nullopt;
    return index;
}

constexpr float chainRows(int n) { return n <= 0 ? 0 : n * kSlotH + (n - 1) * kSlotGap; }
constexpr float panelHeight(int slots) {
    return kGripH + kHeaderH + kStripChrome + kFaderH + chainRows(slots);
}
constexpr int kDefaultSlots = 5, kMinSlots = 2;

std::string dbText(double db) {
    if (db <= ui::kFaderSilenceDb + 1e-9) return "-inf";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%+.1f", db);
    return buf;
}

std::string sendText(const model::Session& session, const model::Route& route) {
    std::string to = "?";
    if (route.to.track)
        if (const auto* dest = session.tracks.find(*route.to.track))
            to = dest->name.empty() ? route.to.track->str() : dest->name;
    if (route.to.fx) to = route.to.fx->str() + " key";
    return "→ " + to + " " + dbText(route.volumeDb);
}

// A left-button press that becomes a drag once it moves a few pixels; a press
// that never does stays a click for the node (nothing is reported then).
class DragTracker {
  public:
    using Phase = Mixer::DragPhase;
    std::function<void(Phase, ui::Point)> notify;
    bool down() const { return down_; }
    bool dragging() const { return dragging_; }
    void press(ui::Point p) {
        down_ = true;
        dragging_ = false;
        origin_ = p;
    }
    // A move: true while a drag is under way.
    bool move(ui::Point p) {
        if (!down_) return false;
        if (!dragging_) {
            if (std::hypot(p.x - origin_.x, p.y - origin_.y) < kDragThreshold) return false;
            dragging_ = true;
            if (notify) notify(Phase::Begin, p);
        }
        if (notify) notify(Phase::Move, p);
        return true;
    }
    // A release: true when it ended a drag (so it is not a click).
    bool release(ui::Point p) {
        const bool was = dragging_;
        down_ = dragging_ = false;
        if (was && notify) notify(Phase::End, p);
        return was;
    }
    bool cancel() {
        const bool was = dragging_;
        down_ = dragging_ = false;
        if (was && notify) notify(Phase::Cancel, {});
        return was;
    }

  private:
    bool down_ = false, dragging_ = false;
    ui::Point origin_;
};

// A plug-in in a strip's chain: a click opens its editor (a button), a drag
// reorders it within the chain or copies it onto another strip.
class FxSlot : public ui::Button {
  public:
    FxSlot(std::string id, std::string text, std::function<void()> click, std::size_t at)
        : Button(std::move(id), std::move(text), std::move(click)), chainIndex(at) {}
    const std::size_t chainIndex;  // its place in the track's chain
    DragTracker drag;
    bool onPointer(const ui::PointerEvent& e) override {
        const bool left = e.button == 1;
        if (e.type == ui::PointerEvent::Type::Down && left) drag.press(e.pos);
        if (e.type == ui::PointerEvent::Type::Move && drag.move(e.pos)) return true;
        if (e.type == ui::PointerEvent::Type::Up && left && drag.release(e.pos)) {
            // Release the button without a click: the pointer is "outside" it.
            Button::onPointer({e.type, {-1e6f, -1e6f}, e.button, e.clicks, e.mods});
            return true;
        }
        return Button::onPointer(e);
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_ESCAPE && drag.cancel()) {
            Button::onPointer({ui::PointerEvent::Type::Up, {-1e6f, -1e6f}, 1, 1, 0});
            if (root()) root()->capture(nullptr);
            return true;
        }
        return Button::onKey(e);
    }
};

// A strip's plug-ins, one per row, scrolled by the wheel or a bar when the
// chain is longer than the room the panel gives it.
class FxList : public ui::Node {
  public:
    explicit FxList(std::string id) : Node(std::move(id)) {
        clipsChildren = true;
        layout.height = ui::SizeRule::fraction();
        auto bar =
            std::make_unique<ui::ScrollBar>(this->id() + ".bar", ui::ScrollBar::Axis::Vertical);
        bar_ = bar.get();
        bar_->onScroll = [this](float v) {
            scroll_ = v;
            relayout();
            invalidate();
        };
        add(std::move(bar));
    }
    FxSlot& addSlot(std::unique_ptr<FxSlot> slot) {
        FxSlot& ref = *slot;
        slots_.push_back(&ref);
        add(std::move(slot));
        return ref;
    }
    const std::vector<FxSlot*>& slots() const { return slots_; }
    float content() const { return chainRows(static_cast<int>(slots_.size())); }
    void arrange(const ui::Canvas& canvas, const ui::Rect& b) override {
        setBounds(b);
        const bool bar = content() > b.h + 0.5f;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content() - b.h));
        bar_->visible = bar;
        const float w = b.w - (bar ? ui::ScrollBar::kThickness + 2 : 0);
        bar_->setRange(content(), b.h);
        bar_->setValue(scroll_);
        bar_->arrange(canvas,
                      {b.right() - ui::ScrollBar::kThickness, b.y, ui::ScrollBar::kThickness, b.h});
        float y = b.y - scroll_;
        for (FxSlot* slot : slots_) {
            slot->arrange(canvas, {b.x, y, w, kSlotH});
            y += kSlotH + kSlotGap;
        }
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Wheel || content() <= bounds().h + 0.5f) return false;
        scroll_ = std::max(0.0f, scroll_ - e.wheelY * (kSlotH + kSlotGap));
        relayout();
        invalidate();
        return true;
    }

  private:
    std::vector<FxSlot*> slots_;
    ui::ScrollBar* bar_ = nullptr;
    float scroll_ = 0;
};

// The panel's top edge: drag to resize the mixer (the chain sections take the
// difference), double-click to restore the default height.
class ResizeGrip : public ui::Node {
  public:
    ResizeGrip() : Node("mixer.grip") {
        interactive = true;
        layout.height = ui::SizeRule::fixed(kGripH);
    }
    std::function<float()> height;      // the panel's height now
    std::function<void(float)> resize;  // a new height
    std::function<void()> reset;        // the default
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const bool live = hovered() || dragging_;
        c.fillRect(bounds(), live ? t.color("selection") : t.color("panel"));
        c.fillRect({bounds().x, bounds().y, bounds().w, 1}, t.color("rule"));
        if (live)
            c.fillRect({bounds().x + bounds().w / 2 - 16, bounds().y + 1, 32, 2}, t.color("focus"));
    }
    void onHover(bool) override { invalidate(); }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            if (e.clicks >= 2) {
                if (reset) reset();
                return true;
            }
            dragging_ = true;
            pressY_ = e.pos.y;
            start_ = height ? height() : 0;
            invalidate();
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Move && dragging_) {
            if (resize) resize(start_ + (pressY_ - e.pos.y));  // up makes it taller
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Up && dragging_) {
            dragging_ = false;
            invalidate();
            return true;
        }
        return false;
    }

  private:
    bool dragging_ = false;
    float pressY_ = 0, start_ = 0;
};

// A run of strips side by side from `scroll` on (the scrolled ones clip).
class StripGroup : public ui::Node {
  public:
    explicit StripGroup(std::string id, bool clip) : Node(std::move(id)) { clipsChildren = clip; }
    float scroll = 0;
    float content() const {
        float w = 0;
        for (const auto& c : children())
            if (c->visible) w += kStripW + 4;
        return w;
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& b) override {
        setBounds(b);
        float x = b.x - scroll;
        for (const auto& c : children()) {
            if (!c->visible) continue;
            c->arrange(canvas, {x, b.y, kStripW, b.h});
            x += kStripW + 4;
        }
    }
};

// Strips side by side, scrolled horizontally (wheel or the bar); the masters
// stay pinned to the right edge.
class StripRow : public ui::Node {
  public:
    StripRow() : Node("mixer.strips") {
        interactive = true;
        layout.height = ui::SizeRule::fraction();
        scrolled_ = &emplace<StripGroup>("mixer.scrolled", true);
        pinned_ = &emplace<StripGroup>("mixer.pinned", false);
        auto bar = std::make_unique<ui::ScrollBar>("mixer.scroll", ui::ScrollBar::Axis::Horizontal);
        bar_ = bar.get();
        bar_->onScroll = [this](float v) {
            scrolled_->scroll = v;
            relayout();
            invalidate();
        };
        add(std::move(bar));
    }
    void clear() {
        for (StripGroup* g : {scrolled_, pinned_}) {
            std::vector<ui::Node*> old;
            for (const auto& c : g->children()) old.push_back(c.get());
            for (ui::Node* n : old) (void)g->remove(n);
        }
    }
    void addStrip(std::unique_ptr<ui::Node> strip, bool pinned) {
        (pinned ? pinned_ : scrolled_)->add(std::move(strip));
    }
    void arrange(const ui::Canvas& canvas, const ui::Rect& b) override {
        setBounds(b);
        const float pinnedW = std::min(b.w, pinned_->content());
        const float areaW = b.w - pinnedW;
        const float content = scrolled_->content();
        const float barH = content > areaW ? ui::ScrollBar::kThickness : 0;
        scrolled_->scroll = std::clamp(scrolled_->scroll, 0.0f, std::max(0.0f, content - areaW));
        bar_->setRange(content, areaW);
        bar_->setValue(scrolled_->scroll);
        bar_->visible = barH > 0;
        bar_->arrange(canvas, {b.x, b.bottom() - barH, areaW, barH});
        scrolled_->arrange(canvas, {b.x, b.y, areaW, b.h - barH});
        pinned_->arrange(canvas, {b.right() - pinnedW, b.y, pinnedW, b.h});
    }
    void paint(ui::Canvas& c) override { c.fillRect(bounds(), c.theme().color("ground")); }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type != ui::PointerEvent::Type::Wheel) return false;
        scrolled_->scroll = std::max(0.0f, scrolled_->scroll - (e.wheelY + e.wheelX) * 60);
        relayout();
        invalidate();
        return true;
    }

  private:
    void relayout() {
        if (ui::Root* r = root()) r->relayout();
    }
    StripGroup* scrolled_ = nullptr;
    StripGroup* pinned_ = nullptr;
    ui::ScrollBar* bar_ = nullptr;
};

// The track's colour, as a stripe under the strip's name (nothing when none).
class ColorStripe : public ui::Node {
  public:
    ColorStripe(std::string id, std::string colour)
        : Node(std::move(id)), colour_(std::move(colour)) {
        layout.height = ui::SizeRule::fixed(3);
    }
    void paint(ui::Canvas& c) override {
        if (const auto colour = c.theme().trackColor(colour_)) c.fillRect(bounds(), *colour);
    }

  private:
    std::string colour_;
};

// A strip's background: the type's colour along the top; a right-click anywhere
// on it opens the track menu, and a drag from a part no control uses moves the
// strip among its own kind.
class StripFrame : public ui::Node {
  public:
    StripFrame(std::string id, std::string colorToken, std::function<void(ui::Point)> menu)
        : Node(std::move(id)), color_(std::move(colorToken)), menu_(std::move(menu)) {
        interactive = true;
        layout.dir = ui::Layout::Dir::Column;
        layout.padding = ui::Insets::all(5);
        layout.gap = 4;
    }
    DragTracker drag;  // notify unset: this strip cannot be dragged
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.fillRect(bounds(), t.color("panel"));
        c.fillRect({bounds().x, bounds().y, bounds().w, 4}, t.color(color_));
        c.fillRect({bounds().right() - 1, bounds().y, 1, bounds().h}, t.color("rule"));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 3 && menu_) {
            menu_(e.pos);
            return true;
        }
        if (!drag.notify) return false;
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            drag.press(e.pos);
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Move && drag.down()) {
            const bool was = drag.dragging();
            const bool now = drag.move(e.pos);
            if (now && !was && root()) root()->focus(this);  // Escape cancels
            return now;
        }
        if (e.type == ui::PointerEvent::Type::Up && e.button == 1 && drag.down()) {
            (void)drag.release(e.pos);
            if (root() && root()->focused() == this) root()->focus(nullptr);
            return true;
        }
        return false;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_ESCAPE && drag.cancel()) {
            if (root()) {
                root()->capture(nullptr);
                root()->focus(nullptr);
            }
            return true;
        }
        return false;
    }

  private:
    std::string color_;
    std::function<void(ui::Point)> menu_;
};

// The drag's feedback, over everything: the dragged strip dimmed, an insertion
// bar where it would land, or the strip a plug-in would be copied onto; a plug-in
// drag carries a chip with its name.
class DragGhost : public ui::Node {
  public:
    explicit DragGhost(std::shared_ptr<const Mixer::DragView> view)
        : Node("mixer.drag"), view_(std::move(view)) {}
    void arrange(const ui::Canvas&, const ui::Rect&) override {
        if (parent()) setBounds(parent()->bounds());
    }
    void paint(ui::Canvas& c) override {
        const Mixer::DragView& d = *view_;
        if (!d.active) return;
        const ui::Theme& t = c.theme();
        const ui::Color accent = t.color("focus");
        c.fillRect(d.source, t.color("ground").withAlpha(0.45f));
        if (d.drop == Mixer::DragView::Drop::Reorder) {
            c.fillRect(d.marker, accent);
        } else if (d.drop == Mixer::DragView::Drop::Copy) {
            c.fillRect(d.marker, accent.withAlpha(0.18f));
            c.strokeRect(d.marker.inset(0.5f), accent, 2);
        }
        if (!d.label.empty()) {
            const float size = t.type("small");
            const float w = c.textWidth(d.label, ui::Font::UiBold, size) + 16;
            const ui::Rect chip{d.pos.x + 12, d.pos.y + 10, w, kSlotH + 2};
            c.fillRound(chip, t.radius("s"), t.color("raised"));
            c.strokeRound(chip, t.radius("s"), accent);
            c.text(chip, d.label, ui::Font::UiBold, size, t.color("ink"),
                   ui::kCenter | ui::kMiddle);
        }
    }

  private:
    std::shared_ptr<const Mixer::DragView> view_;
};

}  // namespace

Mixer::Mixer(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
             engine::EngineApp& app, MeterFeed& meters)
    : root_(root),
      actions_(actions),
      dispatcher_(dispatcher),
      app_(app),
      meters_(meters),
      trackMenu_{root, dispatcher, app, [this](const std::string& e) {
                     if (actions_.onError) actions_.onError(e);
                 }} {
    actions_.addUi("ui.mixer", "View: mixer", [this] { toggle(); });
}

Mixer::~Mixer() {
    ghost_.reset();
    for (const Strip& s : strips_) meters_.detach(s.meter);
}

float Mixer::defaultHeight() { return panelHeight(kDefaultSlots); }

float Mixer::maxHeight() const { return maxPanelHeight(panel_, panelHeight(kMinSlots)); }

void Mixer::setHeight(float height) {
    height_ = std::clamp(height, panelHeight(kMinSlots), 4000.0f);
    applyHeight();
}

json::Value Mixer::saveLayout() const {
    json::Value v = json::Value::object();
    v["height"] = static_cast<double>(height_);
    return v;
}

void Mixer::loadLayout(const json::Value& v) {
    if (const json::Value* h = v.find("height"); h && h->isNumber() && std::isfinite(h->asDouble()))
        setHeight(static_cast<float>(h->asDouble()));
}

// The height shown: the one asked for, within what the window gives now.
void Mixer::applyHeight() {
    const float shown = std::min(height_, maxHeight());
    if (!panel_ || shown == shown_) return;
    shown_ = shown;
    panel_->layout.height = ui::SizeRule::fixed(shown_);
    root_.relayout();
    root_.damageAll();
}

void Mixer::run(std::string_view action, json::Value args) {
    const actions::Action* a = actions_.registry().find(action);
    if (!a) return;
    if (auto r = actions_.run(*a, args); !r && actions_.onError)
        actions_.onError(r.error().message);
}

std::unique_ptr<ui::Node> Mixer::build() {
    auto panel = std::make_unique<ui::Node>("mixer");
    panel_ = panel.get();
    panel->layout.dir = ui::Layout::Dir::Column;
    shown_ = height_;
    panel->layout.height = ui::SizeRule::fixed(shown_);
    panel->visible = false;
    auto& grip = panel->emplace<ResizeGrip>();
    grip.height = [this] { return shown_; };
    grip.resize = [this](float h) { setHeight(std::min(h, maxHeight())); };
    grip.reset = [this] { setHeight(defaultHeight()); };
    auto& header = panel->emplace<ui::Node>("mixer.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(kHeaderH);
    header.layout.padding = ui::Insets{4, 8, 4, 8};
    header.layout.gap = 8;
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("mixer.title", "Mixer", ui::Font::UiBold, "small");
    console9_ =
        &header.emplace<ui::Toggle>("mixer.console9", "Console 9", "accent", [this](bool on) {
            json::Value args = json::Value::object();
            args["console9"] = on;
            if (auto r = dispatcher_.execute({"project.set", std::move(args), kOrigin});
                !r && actions_.onError)
                actions_.onError("Console 9: " + r.error().message);
        });
    console9_->layout.width = ui::SizeRule::content();
    auto& note =
        header.emplace<ui::Label>("mixer.console9.note",
                                  "Console 9 sums through console-style channel and buss stages, "
                                  "level-matched: unity in sounds like unity out.",
                                  ui::Font::Ui, "caption", "ink-faint");
    note.layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("mixer.close", "Close", [this] { setVisible(false); });
    row_ = &panel->add(std::make_unique<StripRow>());
    return panel;
}

void Mixer::toggle() { setVisible(!visible()); }

bool Mixer::visible() const { return panel_ && panel_->visible; }

void Mixer::setVisible(bool visible) {
    if (!panel_ || panel_->visible == visible) return;
    panel_->visible = visible;
    if (visible) {
        version_ = ~0ull;
        sync();
    }
    root_.relayout();
    root_.damageAll();
}

std::string Mixer::structure() const {
    const model::Session& s = app_.head();
    std::string out;
    std::function<void(model::Id)> walk = [&](model::Id id) {
        const model::Track* t = s.tracks.find(id);
        if (!t) return;
        out += id.str() + ":" + std::string(model::stripTypeName(t->type)) + ":" + t->name + ":" +
               t->color + ":";
        for (model::Id fx : t->chain) {
            const model::FxInstance* f = s.fx.find(fx);
            out += fx.str() + ",";
            if (f)
                out += std::to_string(f->bypass) + std::to_string(f->offline) + ":" +
                       std::to_string(f->label.size()) + ":" + f->label + ":" +
                       std::to_string(f->plugin.name.size()) + ":" + f->plugin.name;
        }
        for (const auto& e : s.routes)
            if (e.value->from.track == id) {
                out += e.id.str() + "," + std::to_string(e.value->mute);
                if (e.value->to.track) out += e.value->to.track->str();
                if (e.value->to.fx) out += e.value->to.fx->str();
            }
        out += ";";
        for (model::Id child : t->children) walk(child);
    };
    for (model::Id m : s.project->masters) walk(m);
    return out;
}

Result<json::Value> Mixer::set(const std::string& track, const char* field, json::Value value,
                               const std::string& gesture) {
    json::Value args = json::Value::object();
    args["track"] = track;
    args[field] = std::move(value);
    cmd::Origin origin = kOrigin;
    origin.gesture = gesture;
    auto r = dispatcher_.execute({"track.set", std::move(args), origin});
    if (!r && actions_.onError) actions_.onError(std::string("mixer: ") + r.error().message);
    return r;
}

void Mixer::rebuild() {
    endDrag(false);  // the nodes it points at are about to go
    for (const Strip& s : strips_) meters_.detach(s.meter);
    strips_.clear();
    // Remove the old strips (the row keeps its scroll bar).
    static_cast<StripRow*>(row_)->clear();
    const model::Session& s = app_.head();
    std::vector<model::Id> order;
    std::function<void(model::Id)> walk = [&](model::Id id) {
        const model::Track* t = s.tracks.find(id);
        if (!t) return;
        order.push_back(id);
        for (model::Id child : views::displayChildren(s, *t)) walk(child);
    };
    for (model::Id m : s.project->masters) walk(m);
    // Grouped by type: channels, busses, masters (each in track order).
    std::vector<model::Id> grouped;
    for (model::StripType type :
         {model::StripType::Channel, model::StripType::Buss, model::StripType::Master})
        for (model::Id id : order)
            if (s.tracks.find(id)->type == type) grouped.push_back(id);
    for (model::Id id : grouped) {
        const model::Track& t = *s.tracks.find(id);
        const std::string tid = id.str();
        const std::string type(model::stripTypeName(t.type));
        Strip st;
        st.track = tid;
        st.type = type;
        auto frame =
            std::make_unique<StripFrame>("mixer.strip." + tid, type, [this, id](ui::Point p) {
                // Strip coordinates are the window's (the row lays them out in place).
                trackMenu_.open(id, {p.x, p.y, 1, 1}, {});
            });
        st.node = frame.get();
        // The name, with a small "fx" at the strip's top right like the TCP's: the
        // plugin browser on an empty chain, the chain otherwise.
        auto& nameRow = frame->emplace<ui::Node>("mixer.namerow." + tid);
        nameRow.layout.dir = ui::Layout::Dir::Row;
        nameRow.layout.height = ui::SizeRule::fixed(16);
        nameRow.layout.align = ui::Layout::Align::Center;
        st.name = &nameRow.emplace<ui::Label>("mixer.name." + tid, t.name.empty() ? type : t.name,
                                              ui::Font::UiBold, "caption");
        st.name->layout.width = ui::SizeRule::fraction();
        const bool empty = t.chain.empty();
        std::string fxText = empty ? "fx" : "fx " + std::to_string(t.chain.size());
        for (model::Id fx : t.chain)
            if (const model::FxInstance* f = s.fx.find(fx); f && (f->bypass || f->offline)) {
                fxText += "~";
                break;
            }
        st.fx = &nameRow.emplace<ui::Button>("mixer.fxadd." + tid, fxText, [this, tid, empty] {
            json::Value args = json::Value::object();
            args["track"] = tid;
            run(empty ? "ui.fx.add" : "ui.fx.chain", std::move(args));
        });
        st.fx->layout.width = ui::SizeRule::fixed(fxText.size() > 2 ? 34.0f : 22.0f);
        st.fx->layout.height = ui::SizeRule::fixed(15);
        frame->emplace<ColorStripe>("mixer.color." + tid, t.color);
        // The plug-in chain (a click opens or closes the plug-in's editor); the
        // one section that takes the room the panel's height gives.
        auto& chain = frame->emplace<FxList>("mixer.fx." + tid);
        st.chain = &chain;
        for (std::size_t at = 0; at < t.chain.size(); ++at) {
            const model::Id fx = t.chain[at];
            const model::FxInstance* f = s.fx.find(fx);
            if (!f) continue;
            std::string label = f->label.empty()
                                    ? (f->plugin.name.empty() ? f->plugin.id : f->plugin.name)
                                    : f->label;
            if (f->offline)
                label = "(" + label + ")";
            else if (f->bypass)
                label = "~" + label;
            auto slot = std::make_unique<FxSlot>(
                "mixer.fx." + fx.str(), label,
                [this, fx] {
                    json::Value args = json::Value::object();
                    args["fx"] = fx.str();
                    run("ui.fx.editor", std::move(args));
                },
                at);
            slot->drag.notify = [this, tid, fx](DragPhase phase, ui::Point p) {
                dragFx(tid, fx, phase, p);
            };
            chain.addSlot(std::move(slot));
        }
        // Everything but masters can be dragged (tr_0 stays first).
        if (id != model::kMaster)
            frame->drag.notify = [this, tid](DragPhase phase, ui::Point p) {
                dragStrip(tid, phase, p);
            };
        // Sends: name and level.
        auto& sends = frame->emplace<ui::Node>("mixer.sends." + tid);
        sends.layout.dir = ui::Layout::Dir::Column;
        sends.layout.height = ui::SizeRule::fixed(2 * 14);
        int shown = 0;
        for (const auto& e : s.routes) {
            if (e.value->from.track != id || shown == 2) continue;
            ++shown;
            sends
                .emplace<ui::Label>("mixer.send." + e.id.str(), sendText(s, *e.value), ui::Font::Ui,
                                    "caption", e.value->mute ? "ink-faint" : "ink-quiet")
                .layout.height = ui::SizeRule::fixed(14);
        }
        // Channels: arm and monitor.
        auto& rec = frame->emplace<ui::Node>("mixer.rec." + tid);
        rec.layout.dir = ui::Layout::Dir::Row;
        rec.layout.height = ui::SizeRule::fixed(20);
        rec.layout.gap = 3;
        if (t.type == model::StripType::Channel) {
            st.arm = &rec.emplace<ui::Toggle>(
                "mixer.arm." + tid, "R", "armed",
                [this, tid](bool on) { (void)set(tid, "armed", json::Value(on)); });
            st.monitor = &rec.emplace<ui::Button>("mixer.monitor." + tid, "Mon", [this, tid] {
                const auto parsed = model::Id::parse(tid);
                const model::Track* tr = parsed ? app_.head().tracks.find(*parsed) : nullptr;
                if (!tr) return;
                const char* next = tr->record.monitor == model::Monitor::Off  ? "on"
                                   : tr->record.monitor == model::Monitor::On ? "auto"
                                                                              : "off";
                (void)set(tid, "monitor", json::Value(std::string(next)));
            });
            st.monitor->layout.height = ui::SizeRule::fixed(20);
            st.monitor->layout.width = ui::SizeRule::fraction();
        }
        st.pan = &frame->emplace<ui::PanControl>("mixer.pan." + tid);
        st.pan->onChange = [this, tid](double pan, ui::Gesture g) {
            if (g == ui::Gesture::Begin) ++gesture_;
            if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
            (void)set(
                tid, "pan", json::Value(pan),
                g == ui::Gesture::Move ? "mixer.pan:" + tid + ":" + std::to_string(gesture_) : "");
        };
        auto& faderRow = frame->emplace<ui::Node>("mixer.faderrow." + tid);
        faderRow.layout.dir = ui::Layout::Dir::Row;
        faderRow.layout.height = ui::SizeRule::fixed(kFaderH);
        faderRow.layout.gap = 4;
        auto& spacer = faderRow.emplace<ui::Node>("mixer.spacer." + tid);
        spacer.layout.width = ui::SizeRule::fraction();
        st.fader = &faderRow.emplace<ui::Fader>("mixer.fader." + tid);
        st.meter = &faderRow.emplace<ui::Meter>("mixer.meter." + tid);
        auto& spacer2 = faderRow.emplace<ui::Node>("mixer.spacer2." + tid);
        spacer2.layout.width = ui::SizeRule::fraction();
        st.fader->onChange = [this, tid](double db, ui::Gesture g) {
            for (Strip& x : strips_)
                if (x.track == tid && x.value) x.value->setText(dbText(db));
            if (g == ui::Gesture::Begin) ++gesture_;
            if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
            (void)set(tid, "volume_db", json::Value(db),
                      g == ui::Gesture::Move
                          ? "mixer.volume:" + tid + ":" + std::to_string(gesture_)
                          : "");
        };
        st.value = &frame->emplace<ui::Label>("mixer.value." + tid, dbText(t.volumeDb),
                                              ui::Font::Mono, "caption", "ink-quiet");
        st.value->align = ui::kCenter | ui::kMiddle;
        st.value->layout.height = ui::SizeRule::fixed(14);
        auto& ms = frame->emplace<ui::Node>("mixer.ms." + tid);
        ms.layout.dir = ui::Layout::Dir::Row;
        ms.layout.height = ui::SizeRule::fixed(20);
        ms.layout.gap = 3;
        st.mute = &ms.emplace<ui::Toggle>(
            "mixer.mute." + tid, "M", "meter-hot",
            [this, tid](bool on) { (void)set(tid, "mute", json::Value(on)); });
        st.mute->layout.width = ui::SizeRule::fraction();
        st.solo = &ms.emplace<ui::Toggle>("mixer.solo." + tid, "S", "meter", [this, tid](bool on) {
            (void)set(tid, "solo", json::Value(std::string(on ? "on" : "off")));
        });
        st.solo->layout.width = ui::SizeRule::fraction();
        meters_.attach(tid, st.meter);
        static_cast<StripRow*>(row_)->addStrip(std::move(frame),
                                               t.type == model::StripType::Master);
        strips_.push_back(st);
    }
    root_.relayout();
    root_.damageAll();
}

// The visible part of the strip under `p` (a strip scrolled under the pinned
// masters or past the row's edge is not there).
const Mixer::Strip* Mixer::stripAt(ui::Point p) const {
    for (const Strip& st : strips_) {
        const ui::Rect clip = st.node->parent() ? st.node->parent()->bounds() : st.node->bounds();
        if (st.node->bounds().intersect(clip).contains(p)) return &st;
    }
    return nullptr;
}

void Mixer::dragStrip(const std::string& track, DragPhase phase, ui::Point p) {
    DragView& d = *drag_;
    if (phase == DragPhase::Cancel) return endDrag(false);
    if (phase == DragPhase::Begin) {
        endDrag(false);
        d.kind = DragView::Kind::Strip;
        d.active = true;
        d.track = track;
        ghost_ = root_.addOverlay(std::make_unique<DragGhost>(drag_));
    }
    if (d.kind != DragView::Kind::Strip) return;
    d.pos = p;
    resolveStripDrop();
    if (phase == DragPhase::End) return endDrag(true);
    root_.requestAnimationFrame();
}

void Mixer::dragFx(const std::string& track, model::Id fx, DragPhase phase, ui::Point p) {
    DragView& d = *drag_;
    if (phase == DragPhase::Cancel) return endDrag(false);
    if (phase == DragPhase::Begin) {
        endDrag(false);
        d.kind = DragView::Kind::Fx;
        d.active = true;
        d.track = track;
        d.fx = fx.str();
        if (ui::Node* slot = root_.find("mixer.fx." + d.fx)) d.label = slot->valueText();
        ghost_ = root_.addOverlay(std::make_unique<DragGhost>(drag_));
    }
    if (d.kind != DragView::Kind::Fx) return;
    d.pos = p;
    resolveFxDrop();
    if (phase == DragPhase::End) return endDrag(true);
    root_.requestAnimationFrame();
}

// A strip dropped between two strips of its own type (a master among masters):
// the index track.move takes, among its parent's children.
void Mixer::resolveStripDrop() {
    DragView& d = *drag_;
    d.drop = DragView::Drop::None;
    const model::Session& s = app_.head();
    const auto dragged = model::Id::parse(d.track);
    const model::Track* track = dragged ? s.tracks.find(*dragged) : nullptr;
    if (!track) return;
    std::vector<const Strip*> group;  // its type, in display order
    std::vector<model::Id> ids;
    for (const Strip& st : strips_) {
        if (st.type != std::string(model::stripTypeName(track->type))) continue;
        group.push_back(&st);
        ids.push_back(*model::Id::parse(st.track));
        if (st.track == d.track)
            d.source = st.node->bounds().intersect(st.node->parent()->bounds());
    }
    const ui::Rect row = row_->bounds();
    if (group.empty() || !row.contains(d.pos)) return;
    // Over a strip of another kind, or well past this kind's strips: nowhere.
    if (const Strip* over = stripAt(d.pos); over && over->type != group[0]->type) return;
    const ui::Rect first = group.front()->node->bounds(), last = group.back()->node->bounds();
    if (d.pos.x < first.x - kStripW / 2 || d.pos.x > last.right() + kStripW / 2) return;
    std::size_t gap = 0;
    for (const Strip* st : group)
        if (st->node->bounds().x + st->node->bounds().w / 2 < d.pos.x) ++gap;
    std::optional<std::size_t> index;
    if (track->type == model::StripType::Master) {
        // Among the masters, tr_0 staying first.
        const auto& masters = s.project->masters;
        std::size_t slot = 0;
        for (std::size_t i = 0; i < gap; ++i)
            if (ids[i] != *dragged) ++slot;
        slot = std::max<std::size_t>(slot, 1);
        const auto at = static_cast<std::size_t>(
            std::find(masters.begin(), masters.end(), *dragged) - masters.begin());
        if (slot != at) index = slot;
    } else {
        // In front of the first strip of its own parent and type after the gap.
        std::optional<model::Id> before;
        for (std::size_t i = gap; i < ids.size() && !before; ++i)
            if (ids[i] != *dragged && s.tracks.find(ids[i])->parent == track->parent)
                before = ids[i];
        index = views::reorderIndex(s, *dragged, before);
    }
    if (!index) return;
    const float x = gap < group.size() ? group[gap]->node->bounds().x - 2
                                       : group.back()->node->bounds().right() + 1;
    d.marker = ui::Rect{x - 1, row.y, 3, row.h}.intersect(row);
    d.index = *index;
    d.drop = DragView::Drop::Reorder;
}

// A plug-in dropped among its strip's plug-ins (reorder) or onto another strip
// (a copy, last in that chain).
void Mixer::resolveFxDrop() {
    DragView& d = *drag_;
    d.drop = DragView::Drop::None;
    const Strip* home = nullptr;
    for (const Strip& st : strips_)
        if (st.track == d.track) home = &st;
    auto* homeList = home ? dynamic_cast<FxList*>(home->chain) : nullptr;
    if (!homeList) return;
    const FxSlot* dragged = nullptr;
    for (const FxSlot* slot : homeList->slots())
        if (slot->id() == "mixer.fx." + d.fx) dragged = slot;
    if (!dragged) return;
    d.source = dragged->bounds().intersect(homeList->bounds());
    const Strip* over = stripAt(d.pos);
    if (!over) return;
    if (over != home) {
        d.drop = DragView::Drop::Copy;
        d.target = over->track;
        d.marker = over->node->bounds().intersect(over->node->parent()->bounds());
        return;
    }
    const auto& slots = homeList->slots();
    std::size_t gap = 0;
    for (const FxSlot* slot : slots)
        if (slot->bounds().y + slot->bounds().h / 2 < d.pos.y) ++gap;
    const auto track = model::Id::parse(d.track);
    const model::Track* t = track ? app_.head().tracks.find(*track) : nullptr;
    if (!t) return;
    const std::size_t chainGap = gap < slots.size() ? slots[gap]->chainIndex : t->chain.size();
    const auto index = chainMoveIndex(dragged->chainIndex, chainGap);
    if (!index) return;
    const float y =
        gap < slots.size() ? slots[gap]->bounds().y - 2 : slots.back()->bounds().bottom() + 0.5f;
    d.marker = ui::Rect{homeList->bounds().x, y - 0.5f, homeList->bounds().w, 3}.intersect(
        homeList->bounds());
    d.index = *index;
    d.drop = DragView::Drop::Reorder;
}

// The drag is over: drop it (one command, one undo step) or let it go.
void Mixer::endDrag(bool commit) {
    const DragView done = *drag_;
    *drag_ = DragView{};
    ghost_.reset();
    if (done.kind == DragView::Kind::None) return;
    root_.requestAnimationFrame();
    if (!commit || done.drop == DragView::Drop::None) return;
    json::Value args = json::Value::object();
    const char* command = "fx.move";
    if (done.kind == DragView::Kind::Strip) {
        command = "track.move";
        args["track"] = done.track;
        args["index"] = static_cast<std::int64_t>(done.index);
    } else if (done.drop == DragView::Drop::Copy) {
        command = "fx.copy";
        args["fx"] = done.fx;
        args["track"] = done.target;
    } else {
        args["fx"] = done.fx;
        args["index"] = static_cast<std::int64_t>(done.index);
    }
    if (auto r = dispatcher_.execute({command, std::move(args), kOrigin}); !r && actions_.onError)
        actions_.onError(std::string("mixer: ") + r.error().message);
}

void Mixer::update() {
    const model::Session& s = app_.head();
    if (console9_) console9_->setOn(s.project->console9);
    for (Strip& st : strips_) {
        const auto id = model::Id::parse(st.track);
        const model::Track* t = id ? s.tracks.find(*id) : nullptr;
        if (!t) continue;
        if (!st.fader->dragging()) {
            st.fader->setValue(t->volumeDb);
            st.value->setText(dbText(t->volumeDb));
        }
        for (const auto& route : s.routes) {
            if (route.value->from.track != *id) continue;
            auto* label = dynamic_cast<ui::Label*>(st.node->find("mixer.send." + route.id.str()));
            if (!label) continue;
            label->setText(sendText(s, *route.value));
        }
        st.pan->setValue(t->pan);
        st.mute->setOn(t->mute);
        st.solo->setOn(t->solo != model::Solo::Off);
        if (st.arm) st.arm->setOn(t->record.armed);
        if (st.monitor)
            st.monitor->setText(t->record.monitor == model::Monitor::Off  ? "Mon off"
                                : t->record.monitor == model::Monitor::On ? "Mon on"
                                                                          : "Mon auto");
    }
}

void Mixer::sync() {
    if (!visible()) return;
    applyHeight();  // the window may have been resized
    if (app_.head().version == version_) return;
    version_ = app_.head().version;
    const std::string now = structure();
    if (now != structure_) {
        structure_ = now;
        rebuild();
    }
    update();
}

}  // namespace supple::gui
