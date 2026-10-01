#include "supple/views/tcp.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/theme.h"
#include "supple/ui/widgets.h"

namespace supple::views {
namespace {

std::string_view typeToken(model::StripType t) {
    switch (t) {
        case model::StripType::Channel:
            return "channel";
        case model::StripType::Buss:
            return "buss";
        case model::StripType::Master:
            return "master";
    }
    return "ink";
}

const char* monitorToken(model::Monitor m) {
    switch (m) {
        case model::Monitor::Off:
            return "off";
        case model::Monitor::On:
            return "on";
        case model::Monitor::Auto:
            return "auto";
    }
    return "off";
}

model::Monitor nextMonitor(model::Monitor m) {
    switch (m) {
        case model::Monitor::Off:
            return model::Monitor::On;
        case model::Monitor::On:
            return model::Monitor::Auto;
        case model::Monitor::Auto:
            return model::Monitor::Off;
    }
    return model::Monitor::Off;
}

json::Value trackArg(const model::Id& track) {
    json::Value v = json::Value::object();
    v["track"] = track.str();
    return v;
}

// One drag, one undo step: every move of a drag carries the same key, and the
// key carries a serial so two drags with nothing between them stay two steps
// (the bus merges consecutive commits that share an origin gesture).
std::string gestureFor(std::string_view what, const model::Id& track, std::uint64_t serial) {
    return std::string(what) + ":" + track.str() + ":" + std::to_string(serial);
}

}  // namespace

// One strip's controls. Rows are pooled: a row is bound to a track and
// re-bound as the scroll moves, so its controls keep their identity.
class TcpPanel::Row : public ui::Node {
  public:
    Row(TcpPanel& panel, std::size_t slot);
    ~Row() override;

    void bind(const model::Id& track, int depth, const model::Track& tr,
              const model::Session& session);
    void refresh(const model::Session& session);  // values only
    void unbind();

    bool bound() const { return track_.has_value(); }
    const model::Id& track() const { return *track_; }
    ui::Node* control(std::string_view what) const;

    void arrange(const ui::Canvas& canvas, const ui::Rect& r) override;
    void paint(ui::Canvas& canvas) override;
    bool onPointer(const ui::PointerEvent& e) override;

  private:
    TcpPanel& panel_;
    std::optional<model::Id> track_;
    int depth_ = 0;
    bool channel_ = true;
    bool renaming_ = false;
    ui::Rect nameRect_{};
    ui::Label* name_ = nullptr;
    ui::TextField* rename_ = nullptr;
    ui::Toggle* mute_ = nullptr;
    ui::Toggle* solo_ = nullptr;
    ui::Toggle* arm_ = nullptr;
    ui::Toggle* monitor_ = nullptr;
    ui::Fader* fader_ = nullptr;
    ui::PanControl* pan_ = nullptr;
    ui::Meter* meter_ = nullptr;
    ui::Label* fx_ = nullptr;
    ui::Label* input_ = nullptr;
};

TcpPanel::Row::Row(TcpPanel& panel, std::size_t slot)
    : Node("tcp.row" + std::to_string(slot), ui::Role::Group), panel_(panel) {
    const std::string id = this->id();
    interactive = true;
    clipsChildren = true;
    name_ = &emplace<ui::Label>(id + ".name", "", ui::Font::UiBold, "small", "ink");
    name_->align = ui::kLeft | ui::kMiddle;
    rename_ = &emplace<ui::TextField>(id + ".rename", "");
    rename_->visible = false;
    rename_->onSubmit = [this](const std::string& text) {
        renaming_ = false;
        rename_->visible = false;
        name_->visible = true;
        name_->relayout();
        if (panel_.cb_.commit && track_.has_value()) {
            json::Value args = trackArg(*track_);
            args["name"] = text;
            panel_.cb_.commit("track.set", std::move(args), {});
        }
        invalidate();
    };
    rename_->onCancel = [this] {
        renaming_ = false;
        rename_->visible = false;
        name_->visible = true;
        name_->relayout();
        invalidate();
    };
    auto setTrack = [this](json::Value args, std::string gesture) {
        if (!panel_.cb_.commit || !track_.has_value()) return;
        panel_.cb_.commit("track.set", std::move(args), std::move(gesture));
    };
    mute_ = &emplace<ui::Toggle>(id + ".mute", "M", "meter-hot", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["mute"] = on;
        setTrack(std::move(args), {});
    });
    solo_ = &emplace<ui::Toggle>(id + ".solo", "S", "meter", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["solo"] = on ? "on" : "off";
        setTrack(std::move(args), {});
    });
    arm_ = &emplace<ui::Toggle>(id + ".arm", "A", "clip", [this, setTrack](bool on) {
        json::Value args = trackArg(*track_);
        args["armed"] = on;
        setTrack(std::move(args), {});
    });
    monitor_ = &emplace<ui::Toggle>(id + ".monitor", "mon", "meter", [this, setTrack](bool) {
        const model::Track* tr =
            track_.has_value() ? panel_.session_.tracks.find(*track_) : nullptr;
        json::Value args = trackArg(*track_);
        args["monitor"] = monitorToken(nextMonitor(tr ? tr->record.monitor : model::Monitor::Off));
        setTrack(std::move(args), {});
    });
    fader_ = &emplace<ui::Fader>(id + ".fader", ui::Fader::Orientation::Horizontal);
    fader_->onChange = [this, setTrack](double db, ui::Gesture g) {
        if (g == ui::Gesture::Begin) ++panel_.gestureSerial_;
        if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
        json::Value args = trackArg(*track_);
        args["volume_db"] = db;
        setTrack(std::move(args), g == ui::Gesture::Move
                                      ? gestureFor("tcp.volume", *track_, panel_.gestureSerial_)
                                      : std::string{});
    };
    pan_ = &emplace<ui::PanControl>(id + ".pan");
    pan_->onChange = [this, setTrack](double pan, ui::Gesture g) {
        if (g == ui::Gesture::Begin) ++panel_.gestureSerial_;
        if (g == ui::Gesture::Begin || g == ui::Gesture::End) return;
        json::Value args = trackArg(*track_);
        args["pan"] = pan;
        setTrack(std::move(args), g == ui::Gesture::Move
                                      ? gestureFor("tcp.pan", *track_, panel_.gestureSerial_)
                                      : std::string{});
    };
    meter_ = &emplace<ui::Meter>(id + ".meter", true);
    fx_ = &emplace<ui::Label>(id + ".fx", "", ui::Font::Mono, "caption", "ink-quiet");
    input_ = &emplace<ui::Label>(id + ".input", "", ui::Font::Mono, "caption", "ink-quiet");
}

TcpPanel::Row::~Row() = default;

void TcpPanel::Row::bind(const model::Id& track, int depth, const model::Track& tr,
                         const model::Session& session) {
    track_ = track;
    depth_ = depth;
    channel_ = tr.type == model::StripType::Channel;
    renaming_ = false;
    rename_->visible = false;
    name_->visible = true;
    refresh(session);
    invalidate();
}

void TcpPanel::Row::refresh(const model::Session& session) {
    const model::Track* tr = track_.has_value() ? session.tracks.find(*track_) : nullptr;
    if (!tr) return;
    const std::string name =
        tr->name.empty() ? std::string(model::stripTypeName(tr->type)) : tr->name;
    name_->setText(name);
    if (!renaming_) rename_->setText(name);
    mute_->setOn(tr->mute);
    solo_->setOn(tr->solo != model::Solo::Off);
    arm_->setOn(tr->record.armed);
    monitor_->setOn(tr->record.monitor != model::Monitor::Off);
    fader_->setValue(tr->volumeDb);
    pan_->setValue(tr->pan);
    int fx = 0, bypassed = 0, offline = 0;
    for (model::Id id : tr->chain)
        if (const model::FxInstance* f = session.fx.find(id)) {
            ++fx;
            bypassed += f->bypass ? 1 : 0;
            offline += f->offline ? 1 : 0;
        }
    std::string fxText = fx == 0 ? "FX \u2014" : "FX " + std::to_string(fx);
    if (bypassed) fxText += " byp";
    if (offline) fxText += " off";
    fx_->setText(fxText);
    // The assigned record input (choosing one needs an input picker: follow-up).
    std::string in = "in \u2014";
    if (tr->record.input)
        if (const model::Route* r = session.routes.find(*tr->record.input);
            r && r->from.hwChannels) {
            in = "in ";
            for (std::size_t i = 0; i < r->from.hwChannels->size(); ++i)
                in += (i ? "/" : "") + std::to_string((*r->from.hwChannels)[i]);
        }
    input_->setText(in);
}

void TcpPanel::Row::unbind() {
    track_.reset();
    visible = false;
}

ui::Node* TcpPanel::Row::control(std::string_view what) const {
    if (what == "name") return name_;
    if (what == "rename") return rename_;
    if (what == "mute") return mute_;
    if (what == "solo") return solo_;
    if (what == "arm") return arm_;
    if (what == "monitor") return monitor_;
    if (what == "fader") return fader_;
    if (what == "pan") return pan_;
    if (what == "meter") return meter_;
    if (what == "fx") return fx_;
    if (what == "input") return input_;
    return nullptr;
}

void TcpPanel::Row::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    setBounds(r);
    const float x = r.x, right = r.right();
    const float indent = 14.0f * static_cast<float>(depth_);
    nameRect_ = {x + 26 + indent, r.y + 2, std::max(20.0f, right - x - 34 - indent), 18};
    name_->arrange(canvas, nameRect_);
    rename_->arrange(canvas, {nameRect_.x, nameRect_.y + 1, nameRect_.w, 17});
    meter_->arrange(canvas, {right - 5, r.y + 3, 4, std::max(4.0f, r.h - 6)});

    const bool controls = r.h >= 40;  // room for the button row
    const bool big = r.h >= 58;       // room for fader and pan
    const float ty = r.y + 22;
    mute_->visible = controls;
    solo_->visible = controls;
    arm_->visible = controls && channel_;
    monitor_->visible = controls && channel_;
    input_->visible = controls && channel_;
    fx_->visible = controls;
    float tx = x + 26 + indent;
    for (ui::Node* n : {static_cast<ui::Node*>(mute_), static_cast<ui::Node*>(solo_),
                        static_cast<ui::Node*>(arm_), static_cast<ui::Node*>(monitor_)}) {
        if (!n->visible) continue;
        const bool wide = n == monitor_;  // "mon" needs a little more room
        n->arrange(canvas, {tx, ty, wide ? 28.0f : 19.0f, 14});
        tx += wide ? 30.0f : 21.0f;
    }
    if (input_->visible) {
        input_->arrange(canvas, {tx, ty, 46, 14});
        tx += 48;
    }
    if (fx_->visible) fx_->arrange(canvas, {tx, ty, std::max(20.0f, right - tx - 8), 14});
    fader_->visible = big;
    pan_->visible = big;
    if (big) {
        fader_->arrange(canvas, {x + 6, r.y + 42, std::max(30.0f, right - x - 64), 14});
        pan_->arrange(canvas, {right - 52, r.y + 42, 42, 14});
    }
}

void TcpPanel::Row::paint(ui::Canvas& c) {
    if (!track_.has_value()) return;
    const ui::Theme& t = c.theme();
    const model::Track* tr = panel_.session_.tracks.find(*track_);
    if (!tr) return;
    const ui::Rect r = bounds();
    c.fillRect(r, t.color("panel"));
    c.fillRect({r.x, r.bottom() - 1, r.w, 1}, t.color("rule").withAlpha(0.5f));
    // Parentage: a bar down the left of a child's row.
    if (depth_ > 0) {
        const float px = r.x + 8 + 14.0f * static_cast<float>(depth_ - 1);
        c.fillRect({px, r.y, 1, r.h}, t.color("rule"));
        c.fillRect({px, r.y + r.h / 2 - 3, 6, 1}, t.color("rule"));
    }
    // The strip-type badge: colour and letter differ per type (ADR-0019, §7).
    const ui::Rect badge{r.x + 6 + 14.0f * static_cast<float>(depth_), r.y + 4, 16, 16};
    c.fillRound(badge, tr->type == model::StripType::Master ? t.radius("s") : 2.0f,
                t.color(typeToken(tr->type)));
    c.text(badge,
           std::string(1, static_cast<char>(std::toupper(model::stripTypeName(tr->type).front()))),
           ui::Font::UiBold, t.type("caption"), t.color("panel"), ui::kCenter | ui::kMiddle);
    // Selection (§4.2): the row is highlighted and outlined.
    if (panel_.cb_.selected && panel_.cb_.selected(*track_)) {
        c.fillRect(r, t.color("selection").withAlpha(0.55f));
        c.strokeRect({r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, t.color("focus"), 1.0f);
    }
}

bool TcpPanel::Row::onPointer(const ui::PointerEvent& e) {
    if (e.type != ui::PointerEvent::Type::Down || !track_.has_value()) return false;
    if (e.clicks >= 2 && nameRect_.contains(e.pos)) {  // rename in place
        renaming_ = true;
        rename_->setText(name_->text());
        rename_->selectAll();
        rename_->visible = true;
        name_->visible = false;
        rename_->relayout();
        name_->relayout();
        if (ui::Root* r = root()) r->focus(rename_);
        invalidate();
        return true;
    }
    if (panel_.cb_.select) panel_.cb_.select(*track_, e.mods);
    return true;
}

// --- the panel ---------------------------------------------------------------------

TcpPanel::TcpPanel(const TrackLayout& rowLayout, const Viewport& view, TcpCallbacks callbacks)
    : Node("tcp", ui::Role::List), layout_(rowLayout), view_(view), cb_(std::move(callbacks)) {
    clipsChildren = true;
    add_ = &emplace<ui::Button>("tcp.add", "+ strip", [this] { openAddMenu(); });
    add_->layout.width = ui::SizeRule::fixed(56);
    add_->layout.height = ui::SizeRule::fixed(18);
}

// The three strip types, offered by the "+" (ADR-0019: exactly these).
void TcpPanel::openAddMenu() {
    ui::Root* r = root();
    if (!r || !cb_.addStrip) return;
    std::vector<ui::MenuItem> items;
    for (const char* type : {"Channel", "Buss", "Master"}) {
        const std::string lower = [&] {
            std::string out(type);
            for (char& ch : out)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return out;
        }();
        items.push_back({type, {}, [this, lower] { cb_.addStrip(lower); }, true, false, false});
    }
    r->openPopup(ui::makeMenu("tcp.addmenu", std::move(items), [r] { r->closePopup(); }),
                 add_->bounds(), ui::Placement::Above);
}

TcpPanel::~TcpPanel() {
    // The app's meter feed must not keep pointers to nodes that are going away.
    if (cb_.meterHidden)
        for (Row* row : rows_)
            if (row->bound()) cb_.meterHidden(meterOf(row));
}

ui::Rect TcpPanel::rowRect(std::size_t row) const {
    return {bounds().x, bounds().y + layout_.yOf(row) - view_.scrollY, bounds().w,
            layout_.heightOf(row)};
}

ui::Meter* TcpPanel::meterOf(Row* row) const {
    return row ? static_cast<ui::Meter*>(row->control("meter")) : nullptr;
}

void TcpPanel::setSession(const model::Session& session) {
    session_ = session;
    bindRows();
    for (Row* row : rows_)
        if (row->visible && row->bound()) row->refresh(session_);
    invalidate();
}

void TcpPanel::refresh() {
    for (Row* row : rows_)
        if (row->bound()) row->refresh(session_);
}

void TcpPanel::bindRows() {
    // The rows the current scroll shows; one pooled row each.
    std::vector<std::size_t> shownRows;
    for (std::size_t i = 0; i < layout_.rowCount(); ++i) {
        const ui::Rect r = rowRect(i);
        if (r.bottom() <= bounds().y) continue;
        if (r.y >= bounds().bottom()) break;
        shownRows.push_back(i);
    }
    while (rows_.size() < shownRows.size()) rows_.push_back(&emplace<Row>(*this, rows_.size()));
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        Row* row = rows_[i];
        if (i >= shownRows.size()) {  // scrolled away: release the meter, keep the node
            if (row->bound() && cb_.meterHidden) cb_.meterHidden(meterOf(row));
            row->unbind();
            continue;
        }
        const TrackRow& spec = layout_.rows()[shownRows[i]];
        const model::Track* tr = session_.tracks.find(spec.track);
        if (!tr) continue;
        const bool rebind = !row->bound() || row->track() != spec.track;
        if (rebind && row->bound() && cb_.meterHidden) cb_.meterHidden(meterOf(row));
        row->visible = true;
        if (rebind) {
            row->bind(spec.track, spec.depth, *tr, session_);
            if (cb_.meterShown) cb_.meterShown(spec.track.str(), meterOf(row));
        }
    }
}

void TcpPanel::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    setBounds(r);
    bindRows();
    for (Row* row : rows_) {
        if (!row->visible || !row->bound()) continue;
        const auto it =
            std::find_if(layout_.rows().begin(), layout_.rows().end(),
                         [&](const TrackRow& spec) { return spec.track == row->track(); });
        if (it == layout_.rows().end()) continue;
        row->arrange(canvas, rowRect(static_cast<std::size_t>(it - layout_.rows().begin())));
    }
    if (add_) add_->arrange(canvas, {r.right() - 62, r.bottom() - 22, 56, 18});  // the panel's foot
}

void TcpPanel::paint(ui::Canvas& c) { c.fillRect(bounds(), c.theme().color("panel")); }

ui::Node* TcpPanel::hitTest(ui::Point p) {
    if (add_ && add_->visible && add_->enabled && add_->bounds().contains(p)) return add_;
    return ui::Node::hitTest(p);
}

ui::Node* TcpPanel::rowNode(const model::Id& track) const {
    for (Row* row : const_cast<TcpPanel*>(this)->rows_)
        if (row->visible && row->bound() && row->track() == track) return row;
    return nullptr;
}

ui::Node* TcpPanel::controlNode(const model::Id& track, std::string_view what) const {
    for (Row* row : const_cast<TcpPanel*>(this)->rows_)
        if (row->visible && row->bound() && row->track() == track) return row->control(what);
    return nullptr;
}

}  // namespace supple::views
