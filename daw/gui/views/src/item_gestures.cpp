#include "supple/views/item_gestures.h"

#include <algorithm>
#include <cmath>

#include "supple/ui/canvas.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"

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
    if (!row) return std::nullopt;
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
        }
        if (root_) root_->capture(&view_);
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
                             model::StripType::Channel;
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
            }
            if (copy_)
                runSteps(items.size() == 1 ? "Copy item" : "Copy items", steps);
            else
                runSteps(steps.size() == 1 ? "Move item" : "Move items", steps);
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
            runSteps("Trim item start", steps);
            return;
        }
        case Drag::TrimRight: {
            if (!anchor || anchor->locked || state_.deltaBeats == 0) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["length"] = anchor->length + state_.deltaBeats;
            runSteps("Trim item end", {{"item.set", std::move(args)}});
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
        case Drag::None:
            return;
    }
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
