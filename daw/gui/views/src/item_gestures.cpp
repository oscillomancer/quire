#include "supple/views/item_gestures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"
#include "supple/views/envelope_lane.h"

namespace supple::views {
namespace {

// An item's start and length in beats, whatever its timebase follows.
double startBeat(const model::Session& s, const model::Item& it) {
    return model::itemSpan(s, it).startBeat;
}
double lengthBeats(const model::Session& s, const model::Item& it) {
    return model::itemLengthBeats(s, it);
}

// Where an envelope's points live on screen: its lane (track, route and
// plugin envelopes) or its item (a take's, gap log #22: positions are beats
// from the item's start).
struct EnvFrame {
    ui::Rect area;
    double offsetBeats = 0;
    EnvScale scale;
    float y(double value) const {
        return area.bottom() - static_cast<float>(scale.toNorm(value)) * area.h;
    }
    double value(float yPos) const {
        return scale.fromNorm(static_cast<double>((area.bottom() - yPos) / area.h));
    }
};
std::optional<EnvFrame> envFrame(const ArrangeView& v, model::Id id, const model::Envelope& env) {
    const model::Session& s = v.session();
    if (env.target.object.kind == model::Kind::Take) {
        const auto scale = envScale(env);
        if (!scale) return std::nullopt;
        for (const auto& e : s.items) {
            if (e.value->activeTake != env.target.object) continue;
            const auto row = v.trackLayout().indexOf(e.value->track);
            if (!row) return std::nullopt;
            const ui::Rect item = const_cast<ArrangeView&>(v).lanes().itemRect(*row, *e.value);
            return EnvFrame{Lanes::takeEnvArea(item), model::itemSpan(s, *e.value).startBeat,
                            *scale};
        }
        return std::nullopt;
    }
    const auto row = v.trackLayout().indexOfEnv(id);
    const auto scale = v.trackLayout().scaleOf(env);
    if (!row || !scale) return std::nullopt;
    return EnvFrame{Lanes::envArea(v.rowRect(*row)), 0.0, *scale};
}

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
            const double os = startBeat(s, o), oe = startBeat(s, o) + lengthBeats(s, o);
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
            const double start = startBeat(s, it) + beatDelta + startDelta;
            const double end = startBeat(s, it) + beatDelta + lengthBeats(s, it) + lenDelta;
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
            case ItemGestures::Drag::Move: {
                std::vector<model::Id> moving = g.selection();
                if (anchor && std::find(moving.begin(), moving.end(), st.anchor) == moving.end())
                    moving.push_back(st.anchor);
                for (model::Id id : moving)
                    if (const model::Item* it = itemOf(s, id))
                        if (auto r = rectFor(*it, st.deltaBeats, st.deltaRows, 0, 0)) {
                            c.fillRect(*r, t.color("focus").withAlpha(0.25f));
                            c.strokeRect(*r, t.color("focus"), 1.5f);
                        }
                break;
            }
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
                            v.xAt(s.timeMap->secondsAt(startBeat(s, *anchor) + st.deltaBeats)) -
                            v.xAt(s.timeMap->secondsAt(startBeat(s, *anchor)));
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
                const model::Envelope* env = st.env ? s.envelopes.find(*st.env) : nullptr;
                auto frame = env ? envFrame(v, *st.env, *env) : std::nullopt;
                if (!frame) break;
                frame->offsetBeats += st.envOffset;
                model::Envelope draft = *env;
                draft.points = st.envDraft;
                float x0 = std::max(lanes.x, frame->area.x),
                      x1 = std::min(lanes.right(), frame->area.right());
                if (const model::AutomationItem* ai =
                        st.autoItem ? s.autoItems.find(*st.autoItem) : nullptr) {
                    x0 = std::max(x0, v.xAt(s.timeMap->secondsAt(st.envOffset)));
                    x1 = std::min(
                        x1, v.xAt(s.timeMap->secondsAt(std::min(st.envOffset + ai->sourceLength,
                                                                ai->position + ai->length))));
                }
                ui::Point prev{};
                for (float x = x0; x <= x1; x += 3) {
                    const double beat =
                        s.timeMap->beatAt(std::max(0.0, v.timeAt(x))) - frame->offsetBeats;
                    const ui::Point p{x, frame->y(envValueAt(draft, beat, 0.0))};
                    if (x > x0) c.line(prev, p, t.color("focus"), 2);
                    prev = p;
                }
                if (st.envIndex < st.envDraft.size()) {
                    const model::EnvPoint& pt = st.envDraft[st.envIndex];
                    const ui::Point at{v.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos)),
                                       frame->y(pt.value)};
                    c.fillRect({at.x - 4, at.y - 4, 8, 8}, t.color("focus"));
                    c.text({at.x + 8, at.y - 16, 120, 14}, frame->scale.text(pt.value),
                           ui::Font::Mono, t.type("caption"), t.color("ink"));
                }
                break;
            }
            case ItemGestures::Drag::AutoItemMove:
            case ItemGestures::Drag::AutoItemLength: {
                const model::AutomationItem* ai =
                    st.autoItem ? s.autoItems.find(*st.autoItem) : nullptr;
                const auto row = ai ? v.trackLayout().indexOfEnv(ai->envelope) : std::nullopt;
                if (!row) break;
                const bool move = st.drag == ItemGestures::Drag::AutoItemMove;
                const double from = ai->position + (move ? st.deltaBeats : 0.0);
                const double to = ai->position + ai->length + st.deltaBeats;
                const ui::Rect lane = v.rowRect(*row);
                const float x0 = v.xAt(s.timeMap->secondsAt(from)),
                            x1 = v.xAt(s.timeMap->secondsAt(to));
                const ui::Rect box{x0, lane.y + 1, std::max(2.0f, x1 - x0), lane.h - 2};
                c.fillRect(box, t.color("selection").withAlpha(0.25f));
                c.strokeRect(box, t.color("focus"));
                break;
            }
            case ItemGestures::Drag::Comp:
                if (anchor && st.compTo > st.compFrom) {
                    const auto row = v.trackLayout().indexOf(anchor->track);
                    if (!row) break;
                    const ui::Rect ir = const_cast<ArrangeView&>(v).lanes().itemRect(*row, *anchor);
                    const ui::Rect lane =
                        Lanes::takeLaneRect(ir, anchor->takes.size(), st.takeLane);
                    const float x0 = v.xAt(s.timeMap->secondsAt(st.compFrom));
                    const float x1 = v.xAt(s.timeMap->secondsAt(st.compTo));
                    c.fillRect({x0, lane.y, x1 - x0, lane.h}, t.color("focus").withAlpha(0.3f));
                    c.strokeRect({x0, lane.y, x1 - x0, lane.h}, t.color("focus"), 1.5f);
                }
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

// The registration releases the ghost; its token also covers deferred deletion.
ItemGestures::~ItemGestures() { *alive_ = nullptr; }

void ItemGestures::installOverlay(ui::Root& root) {
    root_ = &root;
    if (!overlay_) overlay_ = root.addOverlay(std::make_unique<Ghost>(alive_));
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

Result<model::Id> ItemGestures::newMidiItem(model::Id track, double fromBeat,
                                            std::optional<double> toBeat) {
    if (!run) return Error("gui.unwired", "no command runner");
    const model::Session& s = view_.session();
    const int bar = s.timeMap->barBeat(std::max(0.0, fromBeat)).bar;
    const double start = s.timeMap->beatOfBar(bar);
    const double oneBar = s.timeMap->beatOfBar(bar + 1) - start;
    const double length = std::max(oneBar, toBeat.value_or(0.0) - start);
    json::Value args = json::Value::object();
    args["track"] = track.str();
    args["bar"] = static_cast<std::int64_t>(bar);
    args["length"] = length;
    args["notation"] = "voice midi; 1: r:*";  // an empty MIDI source (a bar of rest)
    args["name"] = "MIDI";
    auto r = run("New MIDI item", {{"item.add", std::move(args)}});
    if (!r) return std::move(r).error();
    const json::Value* item = r->find("item");
    const auto id = item && item->isString() ? model::Id::parse(item->asString()) : std::nullopt;
    if (!id) return Error("gui.new_midi", "item.add returned no item");
    selection_ = {*id};
    changed();
    if (onOpenMidi) onOpenMidi(*id);
    return *id;
}

ItemGestures::Clip ItemGestures::clip() const {
    Clip c;
    const model::Session& s = view_.session();
    c.version = s.version;
    if (pinVersion) c.pin = pinVersion(c.version);
    std::vector<model::Id> channels;
    for (const auto& row : view_.trackLayout().rows())
        if (!row.env && row.type == model::StripType::Channel) channels.push_back(row.track);
    std::ptrdiff_t first = static_cast<std::ptrdiff_t>(channels.size());
    for (auto id : selection_) {
        const auto* item = itemOf(s, id);
        if (!item) continue;
        const auto found = std::find(channels.begin(), channels.end(), item->track);
        if (found == channels.end()) continue;
        const auto index = found - channels.begin();
        first = std::min(first, index);
        c.items.push_back({id, startBeat(s, *item), startBeat(s, *item) + lengthBeats(s, *item),
                           item->track, index});
    }
    if (!c.items.empty()) {
        c.anchorTrack = channels[static_cast<std::size_t>(first)];
        for (auto& item : c.items) item.channelOffset -= first;
    }
    return c;
}

void ItemGestures::copySelection() {
    Clip c = clip();
    if (!c.items.empty()) clipboard_ = std::move(c);
}

Status ItemGestures::cutSelection() {
    Clip c = clip();
    if (c.items.empty() || !run) return {};
    std::vector<EditStep> steps;
    for (const auto& e : c.items) {
        json::Value args = json::Value::object();
        args["item"] = e.item.str();
        steps.push_back({"item.remove", std::move(args)});
    }
    auto r = run(c.items.size() == 1 ? "Cut item" : "Cut items", steps);
    if (!r) return std::move(r).error();
    clipboard_ = std::move(c);
    selection_.clear();
    changed();
    return {};
}

Status ItemGestures::paste(double atBeat, std::optional<model::Id> track) {
    return pasteClip(clipboard_, atBeat, track,
                     clipboard_.items.size() == 1 ? "Paste item" : "Paste items");
}

Status ItemGestures::duplicateSelection() {
    Clip c = clip();
    if (c.items.empty()) return {};
    double end = 0;
    for (const auto& e : c.items) end = std::max(end, e.end);
    return pasteClip(c, end, std::nullopt,
                     c.items.size() == 1 ? "Duplicate item" : "Duplicate items");
}

Status ItemGestures::pasteClip(const Clip& c, double atBeat, std::optional<model::Id> track,
                               const std::string& label) {
    if (c.items.empty() || !run) return {};
    std::vector<model::Id> channels;
    for (const auto& row : view_.trackLayout().rows())
        if (!row.env && row.type == model::StripType::Channel) channels.push_back(row.track);
    const auto anchor = std::find(channels.begin(), channels.end(),
                                  track.value_or(c.anchorTrack.value_or(model::Id{})));
    double start = 1e300;
    for (const auto& entry : c.items) start = std::min(start, entry.position);
    json::Value placements = json::Value::array();
    std::size_t dropped = 0;
    for (const auto& e : c.items) {
        const auto target = (anchor - channels.begin()) + e.channelOffset;
        if (anchor == channels.end() || target < 0 ||
            static_cast<std::size_t>(target) >= channels.size()) {
            ++dropped;
            continue;
        }
        json::Value p = json::Value::object();
        p["item"] = e.item.str();
        p["track"] = channels[static_cast<std::size_t>(target)].str();
        p["position"] = std::max(0.0, atBeat + (e.position - start));
        placements.push_back(std::move(p));
    }
    json::Value args = json::Value::object();
    args["version"] = static_cast<std::int64_t>(c.version);
    if (placements.asArray().empty())
        return Error("gui.clipboard.targets",
                     "paste dropped " + std::to_string(dropped) + " items: no target channel", {},
                     "select a target channel with enough following channels");
    args["items"] = std::move(placements);
    auto r = run(label, {{"item.paste", std::move(args)}});
    if (!r) return std::move(r).error();
    selection_.clear();
    if (const json::Value* made = r->find("items"); made && made->isArray())
        for (const json::Value& id : made->asArray())
            if (auto parsed = model::Id::parse(id.asString())) selection_.push_back(*parsed);
    changed();
    if (dropped && onError)
        onError(Error("gui.clipboard.targets", "paste dropped " + std::to_string(dropped) +
                                                   " items without a target channel"));
    return {};
}

double ItemGestures::stretchRate(const model::Item& item, const model::Take& take,
                                 double deltaBeats) const {
    const model::Session& s = view_.session();
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    const double start = s.timeMap->secondsAt(startBeat(s, item));
    const double before = s.timeMap->secondsAt(startBeat(s, item) + lengthBeats(s, item)) - start;
    const double after =
        s.timeMap->secondsAt(startBeat(s, item) + lengthBeats(s, item) + deltaBeats) - start;
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
        if (!h.rect.contains(p) || startBeat(s, it) < bestStart) continue;
        bestStart = startBeat(s, it);
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
        // Below the name bar of an item showing its take lanes: comping.
        if (h.zone == Drag::Move && !h.takeBadge && p.y >= r.y + 15 &&
            Lanes::showsTakeLanes(r, it.takes.size())) {
            const ui::Rect first = Lanes::takeLaneRect(r, it.takes.size(), 0);
            const auto index = static_cast<std::size_t>(std::max(0.0f, (p.y - first.y) / first.h));
            h.takeLane = std::min(index, it.takes.size() - 1);
            h.zone = Drag::Comp;
        }
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
        copy_ = false;
        cycleFade_ = false;
        state_ = State{};
        if (const auto lane = rowAt(e.pos.y); lane && view_.trackLayout().rows()[*lane].env) {
            const model::Id env = *view_.trackLayout().rows()[*lane].env;
            const auto ai = autoItemAt(env, e.pos);
            if (e.button == 3) {
                openAutoItemMenu(env, e.pos);
                return true;
            }
            if (ai && ai->zone != Drag::EnvPoint) {
                state_.drag = ai->zone;
                state_.autoItem = ai->item;
                if (root_) root_->capture(&view_);
                return true;
            }
            return envPress(env, e, ai);
        }
        const auto h = hit(e.pos);
        // A take envelope drawn on the item takes the press near its line.
        if (e.button == 1 && h && e.clicks < 2)
            if (const auto env = takeEnvelopeAt(h->item, e.pos)) return envPress(*env, e);
        pressRow_ = rowAt(e.pos.y).value_or(0);
        if (e.button == 1 && e.clicks >= 2 && !h) {  // empty space on a channel: a new MIDI item
            if (const auto row = rowAt(e.pos.y)) {
                const TrackRow& spec = view_.trackLayout().rows()[*row];
                if (!spec.env && spec.type == model::StripType::Channel) {
                    if (auto made = newMidiItem(spec.track, beatAtX(e.pos.x)); !made && onError)
                        onError(made.error());
                    state_ = State{};
                    return true;
                }
            }
        }
        if (e.button == 3 || !h) {  // a marquee (right-drag anywhere, or from empty space)
            // A right-click that does not drag opens the item's menu instead.
            if (e.button == 3 && h) state_.anchor = h->item;
            state_.drag = Drag::Marquee;
            state_.marquee = {e.pos.x, e.pos.y, 0, 0};
            if (mouse.behaviour("lane.drag", e.mods) == "add") state_.marqueeBase = selection_;
        } else {
            state_.anchor = h->item;
            if (e.clicks >= 2) {
                if (isMidi(activeTake(s, *itemOf(s, h->item))) && onOpenMidi) onOpenMidi(h->item);
                state_ = State{};
                return true;
            }
            state_.drag = h->zone;
            if (h->takeLane) state_.takeLane = *h->takeLane;
            // The mouse map says what this press does with these modifiers.
            std::string_view b;
            if (state_.drag == Drag::Move) {
                b = mouse.behaviour("item.drag", e.mods);
                copy_ = b.starts_with("copy");
                if (b.starts_with("slip")) state_.drag = Drag::Slip;
            } else if (state_.drag == Drag::TrimLeft || state_.drag == Drag::TrimRight) {
                b = mouse.behaviour("item.edge", e.mods);
                state_.stretch = state_.drag == Drag::TrimRight && b.starts_with("stretch") &&
                                 !isMidi(activeTake(s, *itemOf(s, h->item)));
            } else if (state_.drag == Drag::FadeIn || state_.drag == Drag::FadeOut) {
                b = mouse.behaviour("fade", e.mods);
                cycleFade_ = b == "cycle_shape";
            }
            if (!b.empty()) free_ = b.ends_with("_free");
            // A press on an unselected item selects it, unless these modifiers
            // make a click toggle or extend the selection, or the drag copies.
            if (!isSelected(h->item) && !copy_ &&
                mouse.behaviour("item.click", e.mods) == "select") {
                selection_ = {h->item};
                changed();
            }
        }
        if (root_) root_->capture(&view_);
        return true;
    }
    if (e.type == T::Move &&
        (state_.drag == Drag::AutoItemMove || state_.drag == Drag::AutoItemLength)) {
        moved_ = moved_ || std::abs(e.pos.x - press_.x) > 3;
        const model::AutomationItem* ai =
            state_.autoItem ? s.autoItems.find(*state_.autoItem) : nullptr;
        if (ai) {
            const double raw = beatAtX(e.pos.x) - beatAtX(press_.x);
            if (state_.drag == Drag::AutoItemMove)
                state_.deltaBeats = std::max(0.0, snap(ai->position + raw, free_)) - ai->position;
            else
                state_.deltaBeats = std::max(ai->position + kMinLength,
                                             snap(ai->position + ai->length + raw, free_)) -
                                    (ai->position + ai->length);
        }
        if (root_) root_->requestAnimationFrame();
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
                std::vector<model::Id> items = selection_;
                if (!isSelected(state_.anchor)) items.push_back(state_.anchor);
                double d = snap(startBeat(s, *anchor) + raw, free_) - startBeat(s, *anchor);
                for (model::Id id : items)  // nothing before the start
                    if (const model::Item* it = itemOf(s, id)) d = std::max(d, -startBeat(s, *it));
                state_.deltaBeats = d;
                // Rows: every selected item must land on a channel.
                const int dr = static_cast<int>(rowAt(e.pos.y).value_or(pressRow_)) -
                               static_cast<int>(pressRow_);
                bool ok = true;
                for (model::Id id : items) {
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
                        std::min(snap(startBeat(s, *anchor) + raw, free_) - startBeat(s, *anchor),
                                 lengthBeats(s, *anchor) - kMinLength);
                break;
            case Drag::TrimRight:
                if (anchor) {
                    const double end = startBeat(s, *anchor) + lengthBeats(s, *anchor);
                    state_.deltaBeats = std::max(snap(end + raw, free_) - end,
                                                 kMinLength - lengthBeats(s, *anchor));
                }
                break;
            case Drag::Slip:
                state_.deltaBeats = raw;
                break;
            case Drag::FadeIn:
            case Drag::FadeOut:
                if (anchor) {
                    const double startSec = s.timeMap->secondsAt(startBeat(s, *anchor));
                    const double endSec =
                        s.timeMap->secondsAt(startBeat(s, *anchor) + lengthBeats(s, *anchor));
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
            case Drag::Comp:
                if (anchor) {
                    const double start = startBeat(s, *anchor),
                                 end = startBeat(s, *anchor) + lengthBeats(s, *anchor);
                    const double a0 = std::clamp(snap(beatAtX(press_.x), free_), start, end);
                    const double a1 = std::clamp(snap(beatAtX(e.pos.x), free_), start, end);
                    state_.compFrom = std::min(a0, a1);
                    state_.compTo = std::max(a0, a1);
                }
                break;
            case Drag::EnvPoint:
            case Drag::AutoItemMove:
            case Drag::AutoItemLength:
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

Status ItemGestures::deleteSelection() {
    if (selection_.empty() || !run) return {};
    std::vector<EditStep> steps;
    for (model::Id item : selection_) {
        json::Value args = json::Value::object();
        args["item"] = item.str();
        steps.push_back({"item.remove", std::move(args)});
    }
    auto r = run(selection_.size() == 1 ? "Delete item" : "Delete items", steps);
    if (!r) return r.error();
    selection_.clear();
    changed();
    return {};
}

void ItemGestures::openItemMenu(ui::Point at) {
    if (!root_ || selection_.empty()) return;
    std::vector<ui::MenuItem> items;
    ui::MenuItem del;
    del.label = selection_.size() == 1 ? "Delete"
                                       : "Delete " + std::to_string(selection_.size()) + " items";
    del.shortcut = "Delete";
    del.run = [this] {
        if (auto r = deleteSelection(); !r && onError) onError(r.error());
    };
    items.push_back(std::move(del));
    // Timebase: what the selection follows when the tempo changes (item.set
    // timebase, one step; the items keep their place now).
    ui::MenuItem rule;
    rule.separator = true;
    items.push_back(rule);
    const model::Session& s = view_.session();
    const model::Item* first = itemOf(s, selection_.front());
    const model::ItemTimebase now = first ? first->timebase : model::ItemTimebase::Track;
    for (auto [label, value, name] :
         {std::tuple{"Follow the beat", model::ItemTimebase::Beats, "beats"},
          {"Follow time", model::ItemTimebase::Time, "time"},
          {"Follow the beat (start only)", model::ItemTimebase::BeatsPosition, "beats_position"},
          {"Follow the track", model::ItemTimebase::Track, "track"}}) {
        ui::MenuItem m;
        m.label = label;
        m.checked = now == value;
        m.run = [this, timebase = std::string(name)] {
            if (!run) return;
            std::vector<EditStep> steps;
            for (model::Id item : selection_) {
                json::Value args = json::Value::object();
                args["item"] = item.str();
                args["timebase"] = timebase;
                steps.push_back({"item.set", std::move(args)});
            }
            if (auto r = run("Item timebase", steps); !r && onError) onError(r.error());
        };
        items.push_back(std::move(m));
    }
    // The active take's own chain and envelopes (gap log #22).
    const model::Item* item =
        itemOf(s, state_.anchor.kind == model::Kind::Item ? state_.anchor : selection_.front());
    if (item && item->activeTake) {
        const model::Id take = *item->activeTake;
        items.push_back(rule);
        ui::MenuItem fx;
        fx.label = "Take FX\u2026";
        fx.run = [this, take] {
            if (onTakeFx) onTakeFx(take);
        };
        items.push_back(std::move(fx));
        for (auto [label, param] : {std::pair{"Take volume envelope", "volume"},
                                    {"Take pan envelope", "pan"},
                                    {"Take mute envelope", "mute"}}) {
            std::optional<model::Id> env;
            bool shown = false;
            for (const auto& e : s.envelopes)
                if (e.value->target.object == take && e.value->target.param == param) {
                    env = e.id;
                    shown = e.value->visible;
                }
            ui::MenuItem m;
            m.label = label;
            m.checked = env && shown;
            m.run = [this, take, env, shown, param = std::string(param)] {
                if (!run) return;
                json::Value args = json::Value::object();
                std::string command;
                if (env) {  // show or hide it
                    command = "env.set";
                    args["env"] = env->str();
                    args["visible"] = !shown;
                } else {  // a new one, flat at its neutral value from the item's start
                    command = "env.add";
                    args["take"] = take.str();
                    args["param"] = param;
                    json::Value points = json::Value::array(), start = json::Value::object();
                    start["beat"] = 0.0;
                    start["value"] = 0.0;
                    points.push_back(std::move(start));
                    args["points"] = std::move(points);
                }
                if (auto r = run(env ? "Take envelope" : "Add take envelope",
                                 {{command, std::move(args)}});
                    !r && onError)
                    onError(r.error());
            };
            items.push_back(std::move(m));
        }
    }
    root_->openPopup(ui::makeMenu("item.menu", std::move(items), [this] { root_->closePopup(); }),
                     {at.x, at.y, 1, 1});
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
    if ((state_.drag == Drag::AutoItemMove || state_.drag == Drag::AutoItemLength) &&
        state_.autoItem && moved_ && std::abs(state_.deltaBeats) > 1e-9) {
        if (const model::AutomationItem* ai = s.autoItems.find(*state_.autoItem)) {
            json::Value args = json::Value::object();
            args["item"] = state_.autoItem->str();
            if (state_.drag == Drag::AutoItemMove)
                args["position"] = ai->position + state_.deltaBeats;
            else
                args["length"] = ai->length + state_.deltaBeats;
            runSteps(state_.drag == Drag::AutoItemMove ? "Move automation item"
                                                       : "Resize automation item",
                     {{"autoitem.set", std::move(args)}});
        }
        return;
    }
    if (state_.drag == Drag::Comp && anchor && !anchor->locked) {
        // A click hears that take for the whole item; a swipe splits the item
        // at the range and hears that take there (the piece keeps the take
        // order, so the take is chosen by index).
        json::Value activate = json::Value::object();
        activate["index"] = static_cast<std::int64_t>(state_.takeLane);
        const double start = startBeat(s, *anchor),
                     end = startBeat(s, *anchor) + lengthBeats(s, *anchor);
        const bool whole = !moved_ || state_.compTo - state_.compFrom < kMinLength ||
                           (state_.compFrom <= start + 1e-9 && state_.compTo >= end - 1e-9);
        if (whole) {
            if (anchor->activeTake && state_.takeLane < anchor->takes.size() &&
                anchor->takes[state_.takeLane] == *anchor->activeTake)
                return;
            activate["item"] = state_.anchor.str();
            runSteps("Switch take", {{"take.activate", std::move(activate)}});
            return;
        }
        std::vector<EditStep> steps;
        std::string target = state_.anchor.str();
        if (state_.compFrom > start + 1e-9) {
            json::Value split = json::Value::object();
            split["item"] = target;
            split["at"] = state_.compFrom;
            steps.push_back({"item.split", std::move(split)});
            target = "$step" + std::to_string(steps.size() - 1) + ".right";
        }
        if (state_.compTo < end - 1e-9) {
            json::Value split = json::Value::object();
            split["item"] = target;
            split["at"] = state_.compTo;
            steps.push_back({"item.split", std::move(split)});
            target = "$step" + std::to_string(steps.size() - 1) + ".left";
        }
        activate["item"] = target;
        steps.push_back({"take.activate", std::move(activate)});
        runSteps("Comp take", steps);
        return;
    }
    // A click on a fade handle with the cycle_shape modifier (Ctrl) cycles its shape.
    if (!moved_ && anchor && !anchor->locked && cycleFade_ &&
        (state_.drag == Drag::FadeIn || state_.drag == Drag::FadeOut)) {
        static constexpr const char* kShapes[] = {"linear",    "slow", "fast", "slow_fast",
                                                  "fast_slow", "cos",  "exp"};
        const bool in = state_.drag == Drag::FadeIn;
        const auto now =
            static_cast<std::size_t>(in ? anchor->fadeIn.shape : anchor->fadeOut.shape);
        json::Value args = json::Value::object();
        args["item"] = state_.anchor.str();
        args[in ? "fade_in_shape" : "fade_out_shape"] = kShapes[(now + 1) % 7];
        runSteps(in ? "Fade-in shape" : "Fade-out shape", {{"item.set", std::move(args)}});
        return;
    }
    if (!moved_) {  // a click
        if (state_.drag == Drag::Marquee && e.button == 3 &&
            state_.anchor.kind == model::Kind::Item) {
            if (!isSelected(state_.anchor)) {
                selection_ = {state_.anchor};
                changed();
            }
            openItemMenu(e.pos);
            return;
        }
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
        const std::string_view click = mouse.behaviour("item.click", e.mods);
        if (click == "toggle") {
            if (isSelected(state_.anchor))
                std::erase(selection_, state_.anchor);
            else
                selection_.push_back(state_.anchor);
        } else if (click == "extend") {
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
            // The anchor joins the set even if it was not selected (a press
            // whose modifiers copy, toggle or extend does not select it).
            std::vector<model::Id> items = selection_;
            if (!isSelected(state_.anchor)) items.push_back(state_.anchor);
            std::vector<EditStep> steps;
            std::vector<Placed> placed;
            for (model::Id id : items) {
                const model::Item* it = itemOf(s, id);
                if (!it || (it->locked && !copy_)) continue;
                std::optional<model::Id> track;
                if (state_.deltaRows != 0) {
                    const auto row = view_.trackLayout().indexOf(it->track);
                    if (!row) continue;
                    const auto target = static_cast<std::ptrdiff_t>(*row) + state_.deltaRows;
                    const auto& rows = view_.trackLayout().rows();
                    if (target < 0 || static_cast<std::size_t>(target) >= rows.size() ||
                        rows[static_cast<std::size_t>(target)].env ||
                        rows[static_cast<std::size_t>(target)].type != model::StripType::Channel)
                        return;
                    track = rows[static_cast<std::size_t>(target)].track;
                }
                if (copy_) {
                    json::Value dup = json::Value::object();
                    dup["item"] = id.str();
                    dup["to"] = startBeat(s, *it) + state_.deltaBeats;
                    steps.push_back({"item.duplicate", std::move(dup)});
                    placed.push_back({"$step" + std::to_string(steps.size() - 1) + ".id",
                                      std::nullopt, track.value_or(it->track),
                                      startBeat(s, *it) + state_.deltaBeats,
                                      startBeat(s, *it) + state_.deltaBeats + lengthBeats(s, *it)});
                    if (!track) continue;
                    json::Value mv = json::Value::object();
                    mv["item"] = "$step" + std::to_string(steps.size() - 1) + ".id";
                    mv["track"] = track->str();
                    steps.push_back({"item.move", std::move(mv)});
                    continue;
                }
                json::Value args = json::Value::object();
                args["item"] = id.str();
                args["position"] = startBeat(s, *it) + state_.deltaBeats;
                if (track) args["track"] = track->str();
                steps.push_back({"item.move", std::move(args)});
                placed.push_back({id.str(), id, track.value_or(it->track),
                                  startBeat(s, *it) + state_.deltaBeats,
                                  startBeat(s, *it) + state_.deltaBeats + lengthBeats(s, *it)});
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
            double d = state_.deltaBeats;
            auto offsetFor = [&](const model::Take& take, double delta) {
                const double rate = take.playrate > 0 ? take.playrate : 1.0;
                return take.startOffset +
                       (isMidi(&take) ? delta
                                      : s.timeMap->secondsAt(startBeat(s, *anchor) + delta) -
                                            s.timeMap->secondsAt(startBeat(s, *anchor))) *
                           rate;
            };
            for (auto id : anchor->takes)
                if (const auto* take = s.takes.find(id); take && offsetFor(*take, d) < 0) {
                    const double rate = take->playrate > 0 ? take->playrate : 1.0;
                    const double limit =
                        isMidi(take)
                            ? -take->startOffset / rate
                            : s.timeMap->beatAt(s.timeMap->secondsAt(startBeat(s, *anchor)) -
                                                take->startOffset / rate) -
                                  startBeat(s, *anchor);
                    d = std::max(d, limit);
                }
            std::vector<EditStep> steps;
            for (auto id : anchor->takes)
                if (const auto* take = s.takes.find(id)) {
                    json::Value targs = json::Value::object();
                    targs["take"] = id.str();
                    targs["start_offset"] = std::max(0.0, offsetFor(*take, d));
                    steps.push_back({"take.set", std::move(targs)});
                }
            if (d == 0) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["position"] = startBeat(s, *anchor) + d;
            args["length"] = lengthBeats(s, *anchor) - d;
            steps.insert(steps.begin(), {"item.set", std::move(args)});
            if (autoCrossfade)
                addCrossfades(
                    s,
                    {{state_.anchor.str(), state_.anchor, anchor->track, startBeat(s, *anchor) + d,
                      startBeat(s, *anchor) + lengthBeats(s, *anchor)}},
                    steps);
            runSteps("Trim item start", steps);
            return;
        }
        case Drag::TrimRight: {
            if (!anchor || anchor->locked || state_.deltaBeats == 0) return;
            json::Value args = json::Value::object();
            args["item"] = state_.anchor.str();
            args["length"] = lengthBeats(s, *anchor) + state_.deltaBeats;
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
                        {{state_.anchor.str(), state_.anchor, anchor->track, startBeat(s, *anchor),
                          startBeat(s, *anchor) + lengthBeats(s, *anchor) + state_.deltaBeats}},
                        steps);
                runSteps("Stretch item", steps);
                return;
            }
            std::vector<EditStep> steps = {{"item.set", std::move(args)}};
            if (autoCrossfade)
                addCrossfades(
                    s,
                    {{state_.anchor.str(), state_.anchor, anchor->track, startBeat(s, *anchor),
                      startBeat(s, *anchor) + lengthBeats(s, *anchor) + state_.deltaBeats}},
                    steps);
            runSteps("Trim item end", steps);
            return;
        }
        case Drag::Slip: {
            const model::Take* take = anchor ? activeTake(s, *anchor) : nullptr;
            if (!take || anchor->locked || state_.deltaBeats == 0) return;
            const double rate = take->playrate > 0 ? take->playrate : 1.0;
            const double shift =
                isMidi(take) ? state_.deltaBeats * rate
                             : (s.timeMap->secondsAt(startBeat(s, *anchor) + state_.deltaBeats) -
                                s.timeMap->secondsAt(startBeat(s, *anchor))) *
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
        case Drag::Comp:
        case Drag::EnvPoint:
        case Drag::AutoItemMove:
        case Drag::AutoItemLength:
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

bool ItemGestures::envPress(model::Id envId, const ui::PointerEvent& e,
                            std::optional<AutoHit> autoItem) {
    const model::Session& s = view_.session();
    const model::Envelope* env = s.envelopes.find(envId);
    auto frame = env ? envFrame(view_, envId, *env) : std::nullopt;
    if (!frame) return true;
    state_.drag = Drag::EnvPoint;
    state_.env = envId;
    state_.envDraft = env->points;
    // Inside an automation item the press edits the item's points (as played,
    // in the loop pass under the pointer); elsewhere the envelope's own.
    if (const model::AutomationItem* ai = autoItem ? s.autoItems.find(autoItem->item) : nullptr) {
        state_.autoItem = autoItem->item;
        state_.envOffset = autoItem->passStart;
        frame->offsetBeats += autoItem->passStart;
        state_.envDraft = ai->points;
        for (model::EnvPoint& pt : state_.envDraft) pt.value = Lanes::autoItemValue(*ai, pt.value);
    }
    std::optional<std::size_t> nearest;
    float reach = 7;
    for (std::size_t i = 0; i < state_.envDraft.size(); ++i) {
        const model::EnvPoint& pt = state_.envDraft[i];
        const float x = view_.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos));
        const float y = frame->y(pt.value);
        const float d = std::max(std::abs(x - e.pos.x), std::abs(y - e.pos.y));
        if (d < reach) {
            reach = d;
            nearest = i;
        }
    }
    const std::string_view press = mouse.behaviour("envelope", e.mods);
    free_ = press == "edit_free";
    if (nearest && press == "delete") {
        state_.envDraft.erase(state_.envDraft.begin() + static_cast<std::ptrdiff_t>(*nearest));
        commitEnv("Delete envelope point");
        state_ = State{};
        return true;
    }
    if (nearest) {
        state_.envIndex = *nearest;
    } else {  // a new point under the pointer
        model::EnvPoint pt;
        pt.pos = std::max(0.0, snap(beatAtX(e.pos.x), free_) - frame->offsetBeats);
        if (state_.autoItem)
            pt.pos = std::min(pt.pos, s.autoItems.find(*state_.autoItem)->sourceLength);
        pt.value = frame->value(e.pos.y);
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
    const model::Envelope* env = state_.env ? s.envelopes.find(*state_.env) : nullptr;
    auto frame = env ? envFrame(view_, *state_.env, *env) : std::nullopt;
    if (!frame || state_.envIndex >= state_.envDraft.size()) return;
    frame->offsetBeats += state_.envOffset;
    const model::AutomationItem* ai =
        state_.autoItem ? s.autoItems.find(*state_.autoItem) : nullptr;
    auto& pts = state_.envDraft;
    model::EnvPoint& pt = pts[state_.envIndex];
    // A mostly vertical drag keeps the point's time (snapping would move it).
    if (std::abs(e.pos.x - press_.x) >= 4) {
        const double lo = state_.envIndex > 0 ? pts[state_.envIndex - 1].pos : 0.0;
        const double hi = state_.envIndex + 1 < pts.size() ? pts[state_.envIndex + 1].pos
                          : ai                             ? ai->sourceLength
                                                           : 1e12;
        const double raw =
            frame->offsetBeats + state_.envOrigPos + beatAtX(e.pos.x) - beatAtX(press_.x);
        pt.pos = std::clamp(snap(raw, free_) - frame->offsetBeats, lo, hi);
    } else {
        pt.pos = state_.envOrigPos;
    }
    pt.value = frame->value(e.pos.y);
}

std::optional<model::Id> ItemGestures::takeEnvelopeAt(model::Id item, ui::Point p) const {
    const model::Session& s = view_.session();
    const model::Item* it = itemOf(s, item);
    if (!it || !it->activeTake) return std::nullopt;
    for (const auto& e : s.envelopes) {
        const model::Envelope& env = *e.value;
        if (env.target.object != *it->activeTake || !env.visible) continue;
        const auto frame = envFrame(view_, e.id, env);
        if (!frame || !frame->area.contains(p)) continue;
        // Near one of its points, or near its line at the pointer's time.
        for (const model::EnvPoint& pt : env.points) {
            const float x = view_.xAt(s.timeMap->secondsAt(frame->offsetBeats + pt.pos));
            if (std::abs(x - p.x) < 7 && std::abs(frame->y(pt.value) - p.y) < 7) return e.id;
        }
        const double beat = beatAtX(p.x) - frame->offsetBeats;
        if (std::abs(frame->y(envValueAt(env, beat, frame->scale.reference())) - p.y) < 5)
            return e.id;
    }
    return std::nullopt;
}

std::optional<ItemGestures::AutoHit> ItemGestures::autoItemAt(model::Id env, ui::Point p) const {
    const model::Session& s = view_.session();
    const auto row = view_.trackLayout().indexOfEnv(env);
    if (!row) return std::nullopt;
    const ui::Rect lane = view_.rowRect(*row);
    if (!lane.contains(p)) return std::nullopt;
    const double beat = beatAtX(p.x);
    std::optional<AutoHit> found;
    double foundStart = -1;
    for (const auto& e : s.autoItems) {
        const model::AutomationItem& ai = *e.value;
        if (ai.envelope != env) continue;
        const float x0 = view_.xAt(s.timeMap->secondsAt(ai.position)),
                    x1 = view_.xAt(s.timeMap->secondsAt(ai.position + ai.length));
        if (p.x < x0 || p.x > x1 + 3) continue;
        if (ai.position < foundStart) continue;  // a later item covers an earlier one
        foundStart = ai.position;
        AutoHit h;
        h.item = e.id;
        h.zone = p.x >= x1 - kEdge                       ? Drag::AutoItemLength
                 : p.y < lane.y + 1 + Lanes::kAutoHeader ? Drag::AutoItemMove
                                                         : Drag::EnvPoint;
        const double pass = ai.loop && ai.sourceLength > 0
                                ? std::floor(std::max(0.0, beat - ai.position) / ai.sourceLength)
                                : 0.0;
        h.passStart = ai.position + pass * ai.sourceLength;
        found = h;
    }
    return found;
}

void ItemGestures::openAutoItemMenu(model::Id env, ui::Point at) {
    if (!root_) return;
    const model::Session& s = view_.session();
    const auto hitItem = autoItemAt(env, at);
    const model::AutomationItem* ai = hitItem ? s.autoItems.find(hitItem->item) : nullptr;
    std::vector<ui::MenuItem> items;
    auto step = [this](std::string label, std::string command, json::Value args) {
        return [this, label = std::move(label), command = std::move(command),
                args = std::move(args)]() mutable {
            if (!run) return;
            if (auto r = run(label, {{command, std::move(args)}}); !r && onError)
                onError(r.error());
        };
    };
    ui::MenuItem insert;
    insert.label = "Insert automation item";
    {
        json::Value args = json::Value::object();
        args["env"] = env.str();
        args["position"] = std::max(0.0, snap(beatAtX(at.x), free_));
        args["length"] = 4.0;
        insert.run = step("Insert automation item", "autoitem.add", std::move(args));
    }
    items.push_back(std::move(insert));
    if (ai) {
        const std::string id = hitItem->item.str();
        auto ref = [&id] {
            json::Value a = json::Value::object();
            a["item"] = id;
            return a;
        };
        ui::MenuItem rule;
        rule.separator = true;
        items.push_back(rule);
        ui::MenuItem pooled;
        pooled.label = "Duplicate (pooled)";
        pooled.run = step("Duplicate automation item", "autoitem.duplicate", ref());
        items.push_back(std::move(pooled));
        ui::MenuItem copy;
        copy.label = "Duplicate (unpooled copy)";
        {
            json::Value a = ref();
            a["pooled"] = false;
            copy.run = step("Duplicate automation item", "autoitem.duplicate", std::move(a));
        }
        items.push_back(std::move(copy));
        if (ai->pool > 0) {
            ui::MenuItem unpool;
            unpool.label = "Unpool";
            unpool.run = step("Unpool automation item", "autoitem.unpool", ref());
            items.push_back(std::move(unpool));
        }
        ui::MenuItem loop;
        loop.label = "Loop";
        loop.checked = ai->loop;
        {
            json::Value a = ref();
            a["loop"] = !ai->loop;
            loop.run = step("Automation item loop", "autoitem.set", std::move(a));
        }
        items.push_back(std::move(loop));
        items.push_back(rule);
        ui::MenuItem del;
        del.label = "Delete automation item";
        del.run = step("Delete automation item", "autoitem.remove", ref());
        items.push_back(std::move(del));
    }
    root_->openPopup(
        ui::makeMenu("autoitem.menu", std::move(items), [this] { root_->closePopup(); }),
        {at.x, at.y, 1, 1});
}

void ItemGestures::commitEnv(const std::string& label) {
    if (!run || !state_.env) return;
    if (!view_.session().envelopes.contains(*state_.env)) return;
    json::Value args = json::Value::object(), points = json::Value::array();
    const model::AutomationItem* ai =
        state_.autoItem ? view_.session().autoItems.find(*state_.autoItem) : nullptr;
    if (state_.autoItem && !ai) return;
    if (ai)
        args["item"] = state_.autoItem->str();
    else
        args["env"] = state_.env->str();
    for (const model::EnvPoint& pt : state_.envDraft) {
        json::Value p = json::Value::object();
        p["beat"] = pt.pos;
        // Back from the played value to the item's own (amplitude scales it).
        p["value"] = ai && std::abs(ai->amplitude) > 1e-9
                         ? ai->baseline + (pt.value - ai->baseline) / ai->amplitude
                         : pt.value;
        p["shape"] = shapeName(pt.shape);
        if (pt.tension != 0) p["tension"] = pt.tension;
        points.push_back(std::move(p));
    }
    args["points"] = std::move(points);
    if (auto r = run(label, {{ai ? "autoitem.set" : "env.points.set", std::move(args)}});
        !r && onError)
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
