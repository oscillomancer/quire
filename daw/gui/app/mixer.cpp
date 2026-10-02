#include "mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "supple/ui/controls.h"
#include "supple/ui/list.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
constexpr float kStripW = 92;

std::string dbText(double db) {
    if (db <= ui::kFaderSilenceDb + 1e-9) return "-inf";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%+.1f", db);
    return buf;
}

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

// A strip's background: the type's colour along the top, a master's heavier
// spine down its left side, and the track's colour as a stripe under the name.
class StripFrame : public ui::Node {
  public:
    StripFrame(std::string id, std::string colorToken, std::string trackColor)
        : Node(std::move(id)), color_(std::move(colorToken)), trackColor_(std::move(trackColor)) {
        layout.dir = ui::Layout::Dir::Column;
        layout.padding = ui::Insets{8, 5, 5, 5};
        layout.gap = 4;
    }
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        c.fillRect(bounds(), t.color("panel"));
        c.fillRect({bounds().x, bounds().y, bounds().w, 4}, t.color(color_));
        if (color_ == "master")
            c.fillRect({bounds().x, bounds().y, 5, bounds().h}, t.color(color_));
        c.fillRect({bounds().right() - 1, bounds().y, 1, bounds().h}, t.color("rule"));
        // Below the 16 px name, in the 4 px gap before the FX slots.
        if (const auto colour = t.trackColor(trackColor_))
            c.fillRect({bounds().x + 5, bounds().y + 8 + 16 + 0.5f, bounds().w - 10, 3}, *colour);
    }

  private:
    std::string color_, trackColor_;
};

}  // namespace

Mixer::Mixer(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
             engine::EngineApp& app, MeterFeed& meters)
    : root_(root), actions_(actions), dispatcher_(dispatcher), app_(app), meters_(meters) {
    actions_.addUi("ui.mixer", "View: mixer", [this] { toggle(); });
}

Mixer::~Mixer() {
    for (const Strip& s : strips_) meters_.detach(s.meter);
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
    panel->layout.height = ui::SizeRule::fixed(380);
    panel->visible = false;
    auto& header = panel->emplace<ui::Node>("mixer.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(30);
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
            out += fx.str() + (f && (f->bypass || f->offline) ? "-" : "") + ",";
        }
        for (const auto& e : s.routes)
            if (e.value->from.track == id) out += e.id.str() + ",";
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
        for (model::Id child : t->children) walk(child);
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
        auto frame = std::make_unique<StripFrame>("mixer.strip." + tid, type, t.color);
        st.node = frame.get();
        st.name = &frame->emplace<ui::Label>("mixer.name." + tid, t.name.empty() ? tid : t.name,
                                             ui::Font::UiBold, "caption");
        st.name->layout.height = ui::SizeRule::fixed(16);
        // FX slots (a click opens the plug-in's editor), then "+ FX" (the plugin
        // browser) or, past two effects, "+N more…" (the chain).
        auto& fxBox = frame->emplace<ui::Node>("mixer.fx." + tid);
        fxBox.layout.dir = ui::Layout::Dir::Column;
        fxBox.layout.height = ui::SizeRule::fixed(3 * 18 + 2 * 2);
        fxBox.layout.gap = 2;
        int slots = 0;
        for (model::Id fx : t.chain) {
            if (slots == 2) break;
            const model::FxInstance* f = s.fx.find(fx);
            if (!f) continue;
            ++slots;
            std::string label = f->label.empty()
                                    ? (f->plugin.name.empty() ? f->plugin.id : f->plugin.name)
                                    : f->label;
            if (f->offline)
                label = "(" + label + ")";
            else if (f->bypass)
                label = "~" + label;
            auto& b = fxBox.emplace<ui::Button>("mixer.fx." + fx.str(), label, [this, fx] {
                json::Value args = json::Value::object();
                args["fx"] = fx.str();
                run("ui.fx.editor", std::move(args));
            });
            b.layout.height = ui::SizeRule::fixed(18);
            b.layout.width = ui::SizeRule::fraction();
        }
        const std::size_t more = t.chain.size() > 2 ? t.chain.size() - 2 : 0;
        auto& add = fxBox.emplace<ui::Button>(
            "mixer.fxadd." + tid, more ? "+" + std::to_string(more) + " more\u2026" : "+ FX",
            [this, tid, more] {
                json::Value args = json::Value::object();
                args["track"] = tid;
                run(more ? "ui.fx.chain" : "ui.fx.add", std::move(args));
            });
        add.layout.height = ui::SizeRule::fixed(18);
        add.layout.width = ui::SizeRule::fraction();
        // Sends: name and level.
        auto& sends = frame->emplace<ui::Node>("mixer.sends." + tid);
        sends.layout.dir = ui::Layout::Dir::Column;
        sends.layout.height = ui::SizeRule::fixed(2 * 14);
        int shown = 0;
        for (const auto& e : s.routes) {
            if (e.value->from.track != id || shown == 2) continue;
            ++shown;
            std::string to = "?";
            if (e.value->to.track)
                if (const model::Track* d = s.tracks.find(*e.value->to.track))
                    to = d->name.empty() ? e.value->to.track->str() : d->name;
            if (e.value->to.fx) to = e.value->to.fx->str() + " key";
            sends
                .emplace<ui::Label>("mixer.send." + e.id.str(),
                                    "→ " + to + " " + dbText(e.value->volumeDb), ui::Font::Ui,
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
        faderRow.layout.height = ui::SizeRule::fraction();
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
