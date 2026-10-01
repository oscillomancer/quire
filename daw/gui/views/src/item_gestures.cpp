#include "supple/views/item_gestures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "supple/ui/canvas.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"
#include "supple/views/envelope_lane.h"

namespace supple::views {
namespace {

constexpr float kEdge = 6;               // px at an item's ends that trim it
constexpr float kHandle = 9;             // the fade handles' square at the top corners
constexpr double kMinLength = 1.0 / 64;  // beats

const model::Item* itemOf(const model::Session& s, model::Id id) { return s.items.find(id); }
const model::Take* activeTake(const model::Session& s, const model::Item& it) {
    return it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
}
bool isMidi(const model::Take* t) { return t && t->source && t->source->kind == model::Kind::Midi; }

// Where a gesture puts an item: its reference in the steps (an id, or
// "$stepN.id" for a copy made by an earlier step), its track and beats.
struct Placed {
    std::string ref;
    std::optional<model::Id> id;
    model::Id track;
    double start = 0, end = 0;
};

// Crossfades for the overlaps the placed items make with the items they do
// not move: the earlier item fades out and the later fades in over the
// overlap (seconds). An item inside another is left alone.
void addCrossfades(const model::Session& s, const std::vector<Placed>& placed,
                   std::vector<EditStep>& steps) {
    std::map<std::string, std::pair<std::optional<double>, std::optional<double>>> fades;
    auto moving = [&](model::Id id) {
        return std::any_of(placed.begin(), placed.end(),
                           [&](const Placed& p) { return p.id == id; });
    };
    auto seconds = [&](double a, double b) {
        return s.timeMap->secondsAt(b) - s.timeMap->secondsAt(a);
    };
    for (const Placed& p : placed)
        for (const auto& e : s.items) {
            const model::Item& o = *e.value;
            if (o.track != p.track || moving(e.id)) continue;
            const double os = o.position, oe = o.position + o.length;
            if (os < p.start && p.start < oe && oe < p.end) {  // o, then p
                const double len = seconds(p.start, oe);
                fades[e.id.str()].second = len;
                fades[p.ref].first = len;
            } else if (p.start < os && os < p.end && p.end < oe) {  // p, then o
                const double len = seconds(os, p.end);
                fades[p.ref].second = len;
                fades[e.id.str()].first = len;
            }
        }
    for (const auto& [ref, fade] : fades) {
        json::Value args = json::Value::object();
        args["item"] = ref;
        if (fade.first) args["fade_in"] = *fade.first;
        if (fade.second) args["fade_out"] = *fade.second;
        steps.push_back({"item.set", std::move(args)});
    }
}

// The ghost and selection layer on the root's overlay: repainted every frame,
// so a drag never damages the cached content.
class Ghost : public ui::Node {
  public:
    explicit Ghost(std::shared_ptr<ItemGestures*> alive)
        : Node("arrange.ghost"), alive_(std::move(alive)) {
        setBounds({0, 0, 1e6f, 1e6f});
    }
    void paint(ui::Canvas& c) override {
        if (!*alive_) return;  // the view went first (the root outlives it)
        const ItemGestures& g = **alive_;
        const ArrangeView& v = g.view();
        const model::Session& s = v.session();
        const ui::Rect lanes = v.lanesRect();
        if (lanes.w <= 0) return;
        const ui::Theme& t = c.theme();
        c.save();
        c.clip(lanes);
        const auto& st = g.state();
        auto rectFor = [&](const model::Item& it, double beatDelta, int rowDelta, double lenDelta,
                           double startDelta) -> std::optional<ui::Rect> {
            auto row = v.trackLayout().indexOf(it.track);
            if (!row) return std::nullopt;
            const auto target = static_cast<std::ptrdiff_t>(*row) + rowDelta;
            if (target < 0 || static_cast<std::size_t>(target) >= v.trackLayout().rowCount())
                return std::nullopt;
            const ui::Rect r = v.rowRect(static_cast<std::size_t>(target));
            const double start = it.position + beatDelta + startDelta;
            const double end = it.position + beatDelta + it.length + lenDelta;
            const float x0 = v.xAt(s.timeMap->secondsAt(start)),
                        x1 = v.xAt(s.timeMap->secondsAt(end));
            return ui::Rect{x0, r.y + 1, std::max(2.0f, x1 - x0), r.h - 2};
        };
        // The selection.
        for (model::Id id : g.selection())
            if (const model::Item* it = itemOf(s, id))
                if (auto r = rectFor(*it, 0, 0, 0, 0)) c.strokeRect(*r, t.color("focus"), 2);
        // The drag's ghost.
        const model::Item* anchor =
            st.anchor.kind == model::Kind::Item ? itemOf(s, st.anchor) : nullptr;
        switch (st.drag) {
            case ItemGestures::Drag::Move:
                for (model::Id id : g.selection())
                    if (const model::Item* it = itemOf(s, id))
                        if (auto r = rectFor(*it, st.deltaBeats, st.deltaRows, 0, 0)) {
                            c.fillRect(*r, t.color("focus").withAlpha(0.25f));
                            c.strokeRect(*r, t.color("focus"), 1.5f);
                        }
                break;
            case ItemGestures::Drag::TrimLeft:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, 0, st.deltaBeats)) {
                        c.fillRect(*r, t.color("focus").withAlpha(0.2f));
                        c.fillRect({r->x, r->y, 2, r->h}, t.color("focus"));
                    }
                break;
            case ItemGestures::Drag::TrimRight:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, st.deltaBeats, 0)) {
                        c.fillRect(*r, t.color("focus").withAlpha(0.2f));
                        c.fillRect({r->right() - 2, r->y, 2, r->h}, t.color("focus"));
                        const model::Take* tk =
                            anchor->activeTake ? s.takes.find(*anchor->activeTake) : nullptr;
                        if (st.stretch && tk) {  // the rate the take will play at
                            char buf[32];
                            std::snprintf(buf, sizeof buf, "%.3gx",
                                          g.stretchRate(*anchor, *tk, st.deltaBeats));
                            c.text({r->right() - 70, r->y + 2, 64, 14}, buf, ui::Font::Mono,
                                   t.type("caption"), t.color("ink"), ui::kRight | ui::kMiddle);
                        }
                    }
                break;
            case ItemGestures::Drag::Slip:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, 0, 0)) {
                        const float dx =
                            v.xAt(s.timeMap->secondsAt(anchor->position + st.deltaBeats)) -
                            v.xAt(s.timeMap->secondsAt(anchor->position));
                        c.strokeRect(*r, t.color("focus"), 1.5f);
                        c.line({r->x + r->w / 2, r->y + r->h / 2},
                               {r->x + r->w / 2 + dx, r->y + r->h / 2}, t.color("focus"), 2);
                    }
                break;
            case ItemGestures::Drag::FadeIn:
            case ItemGestures::Drag::FadeOut:
                if (anchor)
                    if (auto r = rectFor(*anchor, 0, 0, 0, 0)) {
                        const float w =
                            static_cast<float>(st.fadeSeconds * v.viewport().pixelsPerSecond);
                        if (st.drag == ItemGestures::Drag::FadeIn)
                            c.line({r->x, r->bottom()}, {r->x + w, r->y}, t.color("focus"), 2);
                        else
                            c.line({r->right() - w, r->y}, {r->right(), r->bottom()},
                                   t.color("focus"), 2);
                    }
                break;
            case ItemGestures::Drag::Marquee:
                c.fillRect(st.marquee, t.color("selection").withAlpha(0.3f));
                c.strokeRect(st.marquee, t.color("focus"));
                break;
            case ItemGestures::Drag::EnvPoint: {
                const auto row = st.env ? v.trackLayout().indexOfEnv(*st.env) : std::nullopt;
                const model::Envelope* env = st.env ? s.envelopes.find(*st.env) : nullptr;
                const auto scale = env ? v.trackLayout().scaleOf(*env) : std::nullopt;
                if (!row || !scale) break;
                model::Envelope draft = *env;
                draft.points = st.envDraft;
                const ui::Rect area = Lanes::envArea(v.rowRect(*row));
                auto yOf = [&](double value) {
                    return area.bottom() - static_cast<float>(scale->toNorm(value)) * area.h;
                };
                ui::Point prev{};
                for (float x = lanes.x; x <= lanes.right(); x += 3) {
                    const double beat = s.timeMap->beatAt(std::max(0.0, v.timeAt(x)));
                    const ui::Point p{x, yOf(envValueAt(draft, beat, 0.0))};
                    if (x > lanes.x) c.line(prev, p, t.color("focus"), 2);
                    prev = p;
                }
                if (st.envIndex < st.envDraft.size()) {
                    const model::EnvPoint& pt = st.envDraft[st.envIndex];
                    const ui::Point at{v.xAt(s.timeMap->secondsAt(pt.pos)), yOf(pt.value)};
                    c.fillRect({at.x - 4, at.y - 4, 8, 8}, t.color("focus"));
                    c.text({at.x + 8, at.y - 16, 120, 14}, scale->text(pt.value), ui::Font::Mono,
                           t.type("caption"), t.color("ink"));
                }
                break;
            }
            case ItemGestures::Drag::None:
                break;
        }
        c.restore();
    }

  private:
    std::shared_ptr<ItemGestures*> alive_;
};

}  // namespace

ItemGestures::ItemGestures(ArrangeView& view)
    : view_(view), alive_(std::make_shared<ItemGestures*>(this)) {}

// The ghost stays on the overlay (the root may already have destroyed it, so
// it is not touched here); it paints nothing once this is gone.
ItemGestures::~ItemGestures() { *alive_ = nullptr; }

void ItemGestures::installOverlay(ui::Root& root) {
    root_ = &root;
    root.overlay().add(std::make_unique<Ghost>(alive_));
}

void ItemGestures::setSelection(std::vector<model::Id> items) {
    selection_ = std::move(items);
    if (root_) root_->requestAnimationFrame();
}

bool ItemGestures::isSelected(model::Id item) const {
    return std::find(selection_.begin(), selection_.end(), item) != selection_.end();
}

void ItemGestures::changed() {
    if (root_) root_->requestAnimationFrame();
    if (onSelection) onSelection(selection_);
}

double ItemGestures::stretchRate(const model::Item& item, const model::Take& take,
                                 double deltaBeats) const {
    const model::Session& s = view_.session();
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    const double start = s.timeMap->secondsAt(item.position);
    const double before = s.timeMap->secondsAt(item.position + item.length) - start;
    const double after = s.timeMap->secondsAt(item.position + item.length + deltaBeats) - start;
    return std::clamp(after > 0 ? rate * before / after : rate, 0.01, 100.0);
}

double ItemGestures::gridBeats() const {
    const model::Session& s = view_.session();
    const double secondsPerBeat = std::max(1e-6, s.timeMap->secondsAt(1) - s.timeMap->secondsAt(0));
    const double pxPerBeat = view_.viewport().pixelsPerSecond * secondsPerBeat;
    for (double g : {1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0})
        if (g * pxPerBeat >= 10) return g;
    return 32.0;
}

double ItemGestures::beatAtX(float x) const {
    return view_.session().timeMap->beatAt(std::max(0.0, view_.timeAt(x)));
}

double ItemGestures::snap(double beat, bool free) const {
    if (free) return beat;
    const double g = gridBeats();
    return std::round(beat / g) * g;
}

std::optional<std::size_t> ItemGestures::rowAt(float y) const {
    for (std::size_t r = 0; r < view_.trackLayout().rowCount(); ++r) {
        const ui::Rect rr = view_.rowRect(r);
        if (y >= rr.y && y < rr.bottom()) return r;
    }
    return std::nullopt;
}

std::optional<ItemGestures::Hit> ItemGestures::hit(ui::Point p) const {
    if (!view_.lanesRect().contains(p)) return std::nullopt;
    const auto row = rowAt(p.y);
    if (!row || view_.trackLayout().rows()[*row].env) return std::nullopt;  // a lane has no items
    const model::Session& s = view_.session();
    const model::Id track = view_.trackLayout().rows()[*row].track;
    std::optional<Hit> best;
    double bestStart = -1e300;
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != track) continue;
        Hit h;
        h.item = e.id;
        h.row = *row;
        h.rect = const_cast<ArrangeView&>(view_).lanes().itemRect(*row, it);
        if (!h.rect.contains(p) || it.position < bestStart) continue;
        bestStart = it.position;
        const ui::Rect& r = h.rect;
        const bool roomy = r.w > 4 * kHandle;
        if (it.takes.size() > 1 && p.x >= r.right() - 24 && p.x < r.right() - kHandle &&
            p.y < r.y + 16)
            h.takeBadge = true;
        if (roomy && p.y < r.y + kHandle && p.x < r.x + kHandle)
            h.zone = Drag::FadeIn;
        else if (roomy && p.y < r.y + kHandle && p.x > r.right() - kHandle)
            h.zone = Drag::FadeOut;
        else if (r.w > 3 * kEdge && p.x < r.x + kEdge)
            h.zone = Drag::TrimLeft;
        else if (r.w > 3 * kEdge && p.x > r.right() - kEdge)
            h.zone = Drag::TrimRight;
        else
            h.zone = Drag::Move;
        best = h;
    }
    return best;
}

bool ItemGestures::pointer(const ui::PointerEvent& e) {
    using T = ui::PointerEvent::Type;
    const model::Session& s = view_.session();
    if (e.type == T::Down && (e.button == 1 || e.button == 3)) {
        if (!view_.lanesRect().contains(e.pos)) return false;
        press_ = e.pos;
        moved_ = false;
        free_ = (e.mods & ui::kShift) != 0;
        copy_ = (e.mods & ui::kCtrl) != 0;
        state_ = State{};
        if (const auto lane = rowAt(e.pos.y); lane && view_.trackLayout().rows()[*lane].env)
            return e.button == 1 ? envPress(*lane, e) : true;
        const auto h = hit(e.pos);
        pressRow_ = rowAt(e.pos.y).value_or(0);
        if (e.button == 3 || !h) {  // a marquee (right-drag anywhere, or from empty space)
            state_.drag = Drag::Marquee;
            state_.marquee = {e.pos.x, e.pos.y, 0, 0};
            if (e.mods & (ui::kShift | ui::kCtrl)) state_.marqueeBase = selection_;
        } else {
            state_.anchor = h->item;
            if (e.clicks >= 2) {
                if (isMidi(activeTake(s, *itemOf(s, h->item))) && onOpenMidi) onOpenMidi(h->item);
                state_ = State{};
                return true;
            }
            if (!isSelected(h->item) && !(e.mods & (ui::kShift | ui::kCtrl))) {
                selection_ = {h->item};
                changed();
            }
            state_.drag = h->zone;
            if (state_.drag == Drag::Move && (e.mods & ui::kAlt)) state_.drag = Drag::Slip;
            state_.stretch = state_.drag == Drag::TrimRight && (e.mods & ui::kAlt) &&
                             !isMidi(activeTake(s, *itemOf(s, h->item)));
        }
        if (root_) root_->capture(&view_);
        return true;
    }
    if (e.type == T::Move && state_.drag == Drag::EnvPoint) {
        moved_ = moved_ || std::abs(e.pos.x - press_.x) + std::abs(e.pos.y - press_.y) > 3;
        envDrag(e);
        if (root_) root_->requestAnimationFrame();
        return true;
    }
    if (e.type == T::Move && state_.drag != Drag::None) {
        moved_ = moved_ || std::abs(e.pos.x - press_.x) + std::abs(e.pos.y - press_.y) > 3;
        const double raw = beatAtX(e.pos.x) - beatAtX(press_.x);
        const model::Item* anchor =
            state_.anchor.kind == model::Kind::Item ? itemOf(s, state_.anchor) : nullptr;
        switch (state_.drag) {
            case Drag::Move: {
                if (!anchor) break;
                double d = snap(anchor->position + raw, free_) - anchor->position;
                for (model::Id id : selection_)  // nothing before the start
                    if (const model::Item* it = itemOf(s, id)) d = std::max(d, -it->position);
                state_.deltaBeats = d;
                // Rows: every selected item must land on a channel.
                const int dr = static_cast<int>(rowAt(e.pos.y).value_or(pressRow_)) -
                               static_cast<int>(pressRow_);
                bool ok = true;
                for (model::Id id : selection_) {
                    const model::Item* it = itemOf(s, id);
                    const auto row = it ? view_.trackLayout().indexOf(it->track) : std::nullopt;
                    const auto target = row ? static_cast<std::ptrdiff_t>(*row) + dr : -1;
                    ok = ok && target >= 0 &&
                         static_cast<std::size_t>(target) < view_.trackLayout().rowCount() &&
                         view_.trackLayout().rows()[static_cast<std::size_t>(target)].type ==
                             model::StripType::Channel &&
                         !view_.trackLayout().rows()[static_cast<std::size_t>(target)].env;
                }
                state_.deltaRows = ok ? dr : 0;
                break;
            }
            case Drag::TrimLeft:
                if (anchor)
                    state_.deltaBeats =
                        std::min(snap(anchor->position + raw, free_) - anchor->position,
                                 anchor->length - kMinLength);
                break;
            case Drag::TrimRight:
                if (anchor) {
                    const double end = anchor->position + anchor->length;
                    state_.deltaBeats =
                        std::max(snap(end + raw, free_) - end, kMinLength - anchor->length);
                }
                break;
            case Drag::Slip:
                state_.deltaBeats = raw;
                break;
            case Drag::FadeIn:
            case Drag::FadeOut:
                if (anchor) {
                    const double startSec = s.timeMap->secondsAt(anchor->position);
                    const double endSec = s.timeMap->secondsAt(anchor->position + anchor->length);
                    const double at = view_.timeAt(e.pos.x);
                    state_.fadeSeconds =
                        std::clamp(state_.drag == Drag::FadeIn ? at - startSec : endSec - at, 0.0,
                                   endSec - startSec);
                }
                break;
            case Drag::Marquee: {
                state_.marquee = {std::min(press_.x, e.pos.x), std::min(press_.y, e.pos.y),
                                  std::abs(e.pos.x - press_.x), std::abs(e.pos.y - press_.y)};
                selection_ = state_.marqueeBase;
                for (std::size_t r = 0; r < view_.trackLayout().rowCount(); ++r) {
                    if (view_.trackLayout().rows()[r].env) continue;
                    const model::Id track = view_.trackLayout().rows()[r].track;
                    for (const auto& item : s.items) {
                        if (item.value->track != track) continue;
                        const ui::Rect ir = view_.lanes().itemRect(r, *item.value);
                        const ui::Rect& m = state_.marquee;
                        if (ir.x < m.right() && ir.right() > m.x && ir.y < m.bottom() &&
                            ir.bottom() > m.y && !isSelected(item.id))
                            selection_.push_back(item.id);
                    }
                }
                break;
            }
            case Drag::EnvPoint:
            case Drag::None:
                break;
        }
        if (root_) root_->requestAnimationFrame();
        return true;
    }
    if (e.type == T::Up && state_.drag != Drag::None) {
        if (root_) root_->capture(nullptr);
        commit(e);
        state_ = State{};
        if (root_) root_->requestAnimationFrame();
        return true;
    }
    return false;
}

void ItemGestures::commit(const ui::PointerEvent& e) {
    const model::Session& s = view_.session();
    const model::Item* anchor =
        state_.anchor.kind == model::Kind::Item ? itemOf(s, state_.anchor) : nullptr;
    auto runSteps = [&](const std::string& label, const std::vector<EditStep>& steps) {
        if (steps.empty() || !run) return;
        if (auto r = run(label, steps); !r && onError) onError(r.error());
    };
    if (state_.drag == Drag::EnvPoint) {
        if (moved_ || state_.envCreated)
            commitEnv(state_.envCreated ? "Add envelope point" : "Move envelope point");
        return;
    }
    if (!moved_) {  // a click
        if (state_.drag == Drag::Marquee) {
            if (e.button == 1 && state_.marqueeBase.empty()) {
                selection_.clear();
                changed();
                if (onSeek) onSeek(std::max(0.0, view_.timeAt(press_.x)));
            }
            return;
        }
        if (const auto h = hit(press_); h && h->takeBadge && anchor && anchor->activeTake) {
            const auto& takes = anchor->takes;
            auto at = std::find(takes.begin(), takes.end(), *anchor->activeTake);
            const model::Id next =
                (at == takes.end() || at + 1 == takes.end()) ? takes.front() : *(at + 1);
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["take"] = next.str();
            runSteps("Switch take", {{"take.activate", std::move(args)}});
            return;
        }
        if (e.mods & ui::kCtrl) {  // toggle
            if (isSelected(state_.anchor))
                std::erase(selection_, state_.anchor);
            else
                selection_.push_back(state_.anchor);
        } else if (e.mods & ui::kShift) {
            if (!isSelected(state_.anchor)) selection_.push_back(state_.anchor);
        } else {
            selection_ = {state_.anchor};
        }
        changed();
        return;
    }
    switch (state_.drag) {
        case Drag::Marquee:
            changed();
            return;
        case Drag::Move: {
            if (state_.deltaBeats == 0 && state_.deltaRows == 0) return;
            // Ctrl-drag copies: the anchor joins the copied set even if it was
            // not selected (a Ctrl press on an unselected item does not select).
            std::vector<model::Id> items = selection_;
            if (copy_ && !isSelected(state_.anchor)) items.push_back(state_.anchor);
            std::vector<EditStep> steps;
            std::vector<Placed> placed;
            for (model::Id id : items) {
                const model::Item* it = itemOf(s, id);
                if (!it || (it->locked && !copy_)) continue;
                std::optional<model::Id> track;
                if (state_.deltaRows != 0) {
                    const auto row = view_.trackLayout().indexOf(it->track);
                    if (!row) continue;
                    track = view_.trackLayout()
                                .rows()[*row + static_cast<std::size_t>(state_.deltaRows)]
                                .track;
                }
                if (copy_) {
                    json::Value dup = json::Value::object();
                    dup["item"] = id.str();
                    dup["to"] = it->position + state_.deltaBeats;
                    steps.push_back({"item.duplicate", std::move(dup)});
                    placed.push_back({"$step" + std::to_string(steps.size() - 1) + ".id",
                                      std::nullopt, track.value_or(it->track),
                                      it->position + state_.deltaBeats,
                                      it->position + state_.deltaBeats + it->length});
                    if (!track) continue;
                    json::Value mv = json::Value::object();
                    mv["item"] = "$step" + std::to_string(steps.size() - 1) + ".id";
                    mv["track"] = track->str();
                    steps.push_back({"item.move", std::move(mv)});
                    continue;
                }
                json::Value args = json::Value::object();
                args["item"] = id.str();
                args["position"] = it->position + state_.deltaBeats;
                if (track) args["track"] = track->str();
                steps.push_back({"item.move", std::move(args)});
                placed.push_back({id.str(), id, track.value_or(it->track),
                                  it->position + state_.deltaBeats,
                                  it->position + state_.deltaBeats + it->length});
            }
            const std::size_t edits = steps.size();
            if (autoCrossfade) addCrossfades(s, placed, steps);
            if (copy_)
                runSteps(items.size() == 1 ? "Copy item" : "Copy items", steps);
            else
                runSteps(edits == 1 ? "Move item" : "Move items", steps);
            return;
        }
        case Drag::TrimLeft: {
            if (!anchor || anchor->locked || state_.deltaBeats == 0) return;
            const model::Take* take = activeTake(s, *anchor);
            double d = state_.deltaBeats;
            std::vector<EditStep> steps;
            if (take) {
                // The source offset moves with the edge so the material stays put:
                // seconds for audio (at the take's rate), beats for MIDI.
                const double rate = take->playrate > 0 ? take->playrate : 1.0;
                auto offsetFor = [&](double dd) {
                    if (isMidi(take)) return take->startOffset + dd * rate;
                    return take->startOffset + (s.timeMap->secondsAt(anchor->position + dd) -
                                                s.timeMap->secondsAt(anchor->position)) *
                                                   rate;
                };
                if (offsetFor(d) < 0) {  // not before the source's start
                    if (isMidi(take))
                        d = -take->startOffset / rate;
                    else
                        d = s.timeMap->beatAt(s.timeMap->secondsAt(anchor->position) -
                                              take->startOffset / rate) -
                            anchor->position;
                }
                json::Value targs = json::Value::object();
                targs["take"] = anchor->activeTake->str();
                targs["start_offset"] = std::max(0.0, offsetFor(d));
                steps.push_back({"take.set", std::move(targs)});
            }
            if (d == 0) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["position"] = anchor->position + d;
            args["length"] = anchor->length - d;
            steps.insert(steps.begin(), {"item.set", std::move(args)});
            if (autoCrossfade)
                addCrossfades(s,
                              {{state_.anchor.str(), state_.anchor, anchor->track,
                                anchor->position + d, anchor->position + anchor->length}},
                              steps);
            runSteps("Trim item start", steps);
            return;
        }
        case Drag::TrimRight: {
            if (!anchor || anchor->locked || state_.deltaBeats == 0) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["length"] = anchor->length + state_.deltaBeats;
            const model::Take* take = activeTake(s, *anchor);
            if (state_.stretch && take) {
                json::Value rate = json::Value::object();
                rate["take"] = anchor->activeTake->str();
                rate["playrate"] = stretchRate(*anchor, *take, state_.deltaBeats);
                std::vector<EditStep> steps = {{"item.set", std::move(args)},
                                               {"take.set", std::move(rate)}};
                if (autoCrossfade)
                    addCrossfades(
                        s,
                        {{state_.anchor.str(), state_.anchor, anchor->track, anchor->position,
                          anchor->position + anchor->length + state_.deltaBeats}},
                        steps);
                runSteps("Stretch item", steps);
                return;
            }
            std::vector<EditStep> steps = {{"item.set", std::move(args)}};
            if (autoCrossfade)
                addCrossfades(s,
                              {{state_.anchor.str(), state_.anchor, anchor->track, anchor->position,
                                anchor->position + anchor->length + state_.deltaBeats}},
                              steps);
            runSteps("Trim item end", steps);
            return;
        }
        case Drag::Slip: {
            const model::Take* take = anchor ? activeTake(s, *anchor) : nullptr;
            if (!take || anchor->locked || state_.deltaBeats == 0) return;
            const double rate = take->playrate > 0 ? take->playrate : 1.0;
            const double shift = isMidi(take)
                                     ? state_.deltaBeats * rate
                                     : (s.timeMap->secondsAt(anchor->position + state_.deltaBeats) -
                                        s.timeMap->secondsAt(anchor->position)) *
                                           rate;
            json::Value args = json::Value::object();
            args["take"] = anchor->activeTake->str();
            args["start_offset"] = std::max(0.0, take->startOffset - shift);
            runSteps("Slip item", {{"take.set", std::move(args)}});
            return;
        }
        case Drag::FadeIn:
        case Drag::FadeOut: {
            if (!anchor || anchor->locked) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args[state_.drag == Drag::FadeIn ? "fade_in" : "fade_out"] = state_.fadeSeconds;
            runSteps(state_.drag == Drag::FadeIn ? "Fade in" : "Fade out",
                     {{"item.set", std::move(args)}});
            return;
        }
        case Drag::EnvPoint:
        case Drag::None:
            return;
    }
}

namespace {

const char* shapeName(model::PointShape shape) {
    switch (shape) {
        case model::PointShape::Linear:
            return "linear";
        case model::PointShape::Square:
            return "square";
        case model::PointShape::Slow:
            return "slow";
        case model::PointShape::FastStart:
            return "fast_start";
        case model::PointShape::FastEnd:
            return "fast_end";
        case model::PointShape::Bezier:
            return "bezier";
    }
    return "linear";
}
}  // namespace

bool ItemGestures::envPress(std::size_t row, const ui::PointerEvent& e) {
    const model::Session& s = view_.session();
    const model::Id envId = *view_.trackLayout().rows()[row].env;
    const model::Envelope* env = s.envelopes.find(envId);
    const auto scale = env ? view_.trackLayout().scaleOf(*env) : std::nullopt;
    if (!scale) return true;
    const ui::Rect area = Lanes::envArea(view_.rowRect(row));
    state_.drag = Drag::EnvPoint;
    state_.env = envId;
    state_.envDraft = env->points;
    std::optional<std::size_t> nearest;
    float reach = 7;
    for (std::size_t i = 0; i < env->points.size(); ++i) {
        const model::EnvPoint& pt = env->points[i];
        const float x = view_.xAt(s.timeMap->secondsAt(pt.pos));
        const float y = area.bottom() - static_cast<float>(scale->toNorm(pt.value)) * area.h;
        const float d = std::max(std::abs(x - e.pos.x), std::abs(y - e.pos.y));
        if (d < reach) {
            reach = d;
            nearest = i;
        }
    }
    if (nearest && (e.mods & ui::kAlt)) {  // delete it
        state_.envDraft.erase(state_.envDraft.begin() + static_cast<std::ptrdiff_t>(*nearest));
        commitEnv("Delete envelope point");
        state_ = State{};
        return true;
    }
    if (nearest) {
        state_.envIndex = *nearest;
    } else {  // a new point under the pointer
        model::EnvPoint pt;
        pt.pos = std::max(0.0, snap(beatAtX(e.pos.x), free_));
        pt.value = scale->fromNorm(static_cast<double>((area.bottom() - e.pos.y) / area.h));
        const auto at =
            std::upper_bound(state_.envDraft.begin(), state_.envDraft.end(), pt.pos,
                             [](double b, const model::EnvPoint& p) { return b < p.pos; });
        state_.envIndex = static_cast<std::size_t>(at - state_.envDraft.begin());
        state_.envDraft.insert(at, pt);
        state_.envCreated = true;
    }
    state_.envOrigPos = state_.envDraft[state_.envIndex].pos;
    if (root_) {
        root_->capture(&view_);
        root_->requestAnimationFrame();
    }
    return true;
}

void ItemGestures::envDrag(const ui::PointerEvent& e) {
    const model::Session& s = view_.session();
    const auto row = view_.trackLayout().indexOfEnv(*state_.env);
    const model::Envelope* env = s.envelopes.find(*state_.env);
    const auto scale = env ? view_.trackLayout().scaleOf(*env) : std::nullopt;
    if (!row || !scale || state_.envIndex >= state_.envDraft.size()) return;
    const ui::Rect area = Lanes::envArea(view_.rowRect(*row));
    auto& pts = state_.envDraft;
    model::EnvPoint& pt = pts[state_.envIndex];
    // A mostly vertical drag keeps the point's time (snapping would move it).
    if (std::abs(e.pos.x - press_.x) >= 4) {
        const double lo = state_.envIndex > 0 ? pts[state_.envIndex - 1].pos : 0.0;
        const double hi = state_.envIndex + 1 < pts.size() ? pts[state_.envIndex + 1].pos : 1e12;
        const double raw = state_.envOrigPos + beatAtX(e.pos.x) - beatAtX(press_.x);
        pt.pos = std::clamp(snap(raw, free_), lo, hi);
    } else {
        pt.pos = state_.envOrigPos;
    }
    pt.value = scale->fromNorm(static_cast<double>((area.bottom() - e.pos.y) / area.h));
}

void ItemGestures::commitEnv(const std::string& label) {
    if (!run || !state_.env) return;
    json::Value args = json::Value::object(), points = json::Value::array();
    args["env"] = state_.env->str();
    for (const model::EnvPoint& pt : state_.envDraft) {
        json::Value p = json::Value::object();
        p["beat"] = pt.pos;
        p["value"] = pt.value;
        p["shape"] = shapeName(pt.shape);
        if (pt.tension != 0) p["tension"] = pt.tension;
        points.push_back(std::move(p));
    }
    args["points"] = std::move(points);
    if (auto r = run(label, {{"env.points.set", std::move(args)}}); !r && onError)
        onError(r.error());
}

Status ItemGestures::drop(const std::vector<std::filesystem::path>& files, ui::Point at) {
    if (files.empty() || !run) return {};
    const model::Session& s = view_.session();
    const double position =
        view_.lanesRect().contains(at) ? std::max(0.0, snap(beatAtX(at.x), false)) : 0.0;
    const auto row = rowAt(at.y);
    std::vector<EditStep> steps;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string track;
        if (i == 0 && row && view_.trackLayout().rows()[*row].type == model::StripType::Channel) {
            track = view_.trackLayout().rows()[*row].track.str();
        } else {
            json::Value add = json::Value::object();
            add["name"] = files[i].stem().string();
            steps.push_back({"track.add", std::move(add)});
            track =
                "$step" + std::to_string(steps.size() - 1) + ".track";  // the runner substitutes it
        }
        json::Value item = json::Value::object();
        item["track"] = track;
        item["position"] = position;
        item["audio_file"] = files[i].string();
        steps.push_back({"item.add", std::move(item)});
    }
    (void)s;
    auto r = run(files.size() == 1 ? "Drop file" : "Drop files", steps);
    if (!r) return std::move(r).error();
    return {};
}

}  // namespace supple::views
