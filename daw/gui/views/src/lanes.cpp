#include "supple/views/lanes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "supple/time/timeline.h"

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

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

// Item labels paint after the GL layer (a child paints after its parent).
class LaneLabels : public ui::Node {
  public:
    explicit LaneLabels(Lanes& lanes) : Node("lanes-labels"), lanes_(lanes) {}
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const model::Session& s = lanes_.session_;
        for (std::size_t i = 0; i < lanes_.rows_.size(); ++i) {
            const ui::Rect row = lanes_.rowRect(i).intersect(bounds());
            if (row.empty()) continue;
            for (model::Id id : lanes_.rows_[i].items) {
                const model::Item* item = s.items.find(id);
                if (!item) continue;
                const ui::Rect r = lanes_.itemRect(i, *item).intersect(bounds());
                if (r.w < 24) continue;
                const model::Take* take =
                    item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
                std::string name = take ? take->name : std::string();
                if (name.empty()) name = id.str();
                c.text({r.x + 4, r.y + 1, r.w - 8, 14}, name, ui::Font::Ui, t.type("caption"),
                       t.color("ink").withAlpha(0.85f));
            }
        }
    }

  private:
    Lanes& lanes_;
};

Lanes::Lanes(peaks::PeakService& peaks, Resolve resolve)
    : Node("lanes", ui::Role::Custom), peaks_(peaks), resolve_(std::move(resolve)) {
    interactive = true;
    drawsGl = true;
    clipsChildren = true;
    emplace<LaneLabels>(*this);
}

void Lanes::show(const model::Session& session) {
    if (session.version == indexed_) return;
    session_ = session;
    indexed_ = session.version;
    rows_.clear();
    std::function<void(model::Id)> visit = [&](model::Id id) {
        const model::Track* t = session_.tracks.find(id);
        if (!t) return;
        if (t->type != model::StripType::Master && !t->hiddenTcp)
            rows_.push_back({id, t->type, {}});
        for (model::Id c : t->children) visit(c);
    };
    for (model::Id m : session_.project->masters) visit(m);
    for (const auto& e : session_.items) {
        auto row = std::find_if(rows_.begin(), rows_.end(),
                                [&](const Row& r) { return r.track == e.value->track; });
        if (row != rows_.end()) row->items.push_back(e.id);
    }
    for (Row& r : rows_)
        std::sort(r.items.begin(), r.items.end(), [&](model::Id a, model::Id b) {
            return model::itemSpan(session_, *session_.items.find(a)).startSec <
                   model::itemSpan(session_, *session_.items.find(b)).startSec;
        });
    invalidate();
}

void Lanes::setViewport(const Viewport& v) {
    view_ = v;
    view_.pixelsPerSecond = std::clamp(view_.pixelsPerSecond, 0.01, 200000.0);
    view_.startSeconds = std::max(0.0, view_.startSeconds);
    view_.scrollY = std::clamp(view_.scrollY, 0.0f, std::max(0.0f, contentHeight() - bounds().h));
    invalidate();
}

void Lanes::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    setBounds(r);
    for (const auto& c : children()) c->arrange(canvas, r);
}

ui::Rect Lanes::rowRect(std::size_t i) const {
    return {bounds().x, bounds().y + static_cast<float>(i) * view_.rowHeight - view_.scrollY,
            bounds().w, view_.rowHeight};
}

ui::Rect Lanes::itemRect(std::size_t row, const model::Item& item) const {
    const ui::Rect r = rowRect(row);
    const model::ItemSpan span = model::itemSpan(session_, item);
    const float x0 = xAt(span.startSec), x1 = xAt(span.endSec);
    return {x0, r.y + 1, std::max(1.0f, x1 - x0), r.h - 2};
}

void Lanes::paint(ui::Canvas& c) {
    theme_ = &c.theme();
    const ui::Theme& t = c.theme();
    c.fillRect(bounds(), t.color("ground"));
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const ui::Rect r = rowRect(i);
        if (r.bottom() < bounds().y) continue;
        if (r.y > bounds().bottom()) break;
        if (i % 2) c.fillRect(r, t.color("panel").withAlpha(0.5f));
        c.fillRect({r.x, r.bottom() - 1, r.w, 1}, t.color("rule").withAlpha(0.6f));
    }
}

void Lanes::paintGl(const ui::GlFrame& frame) {
    if (glFailed_ || !theme_) return;
    if (!bodies_) {
        auto b = ui::RectRenderer::create();
        auto n = ui::RectRenderer::create();
        auto s = ui::StripRenderer::create();
        if (!b || !n || !s) {
            std::fprintf(stderr, "lanes: GL layers unavailable: %s\n",
                         (!b   ? b.error()
                          : !n ? n.error()
                               : s.error())
                             .toString()
                             .c_str());
            glFailed_ = true;
            return;
        }
        bodies_.emplace(std::move(*b));
        notes_.emplace(std::move(*n));
        strips_.emplace(std::move(*s));
    }
    const ui::Theme& t = *theme_;
    stats_ = {};
    const ui::Rect clip = frame.clip.intersect(bounds());
    const double t0 = timeAt(clip.x), t1 = timeAt(clip.right());
    const std::uint32_t wave = ui::packRgba(t.color("ink").withAlpha(0.8f));
    const std::uint32_t noteBorder = ui::packRgba(t.color("ground").withAlpha(0.7f));
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const ui::Rect row = rowRect(i);
        if (row.bottom() <= clip.y) continue;
        if (row.y >= clip.bottom()) break;
        ++stats_.rows;
        const ui::Color type = t.color(typeToken(rows_[i].type));
        const std::uint32_t body = ui::packRgba(type.withAlpha(0.28f));
        const std::uint32_t edge = ui::packRgba(type.withAlpha(0.85f));
        const std::uint32_t note = ui::packRgba(type);
        for (model::Id id : rows_[i].items) {
            const model::Item* item = session_.items.find(id);
            if (!item) continue;
            const model::ItemSpan span = model::itemSpan(session_, *item);
            if (span.startSec >= t1) break;  // sorted by start
            if (span.endSec <= t0) continue;
            ++stats_.items;
            const ui::Rect r = itemRect(i, *item);
            bodies_->add(r, body, edge);
            const model::Take* take =
                item->activeTake ? session_.takes.find(*item->activeTake) : nullptr;
            if (!take || !take->source) continue;
            const ui::Rect inner{r.x, r.y + 15, r.w, std::max(0.0f, r.h - 17)};  // under the label
            if (const model::MediaSource* m = session_.media.find(*take->source))
                drawAudio(frame, inner, *item, *take, *m, wave);
            else if (const model::MidiSource* s = session_.midi.find(*take->source))
                drawMidi(inner, *item, *take, *s, note, noteBorder);
        }
    }
    bodies_->flush(frame);
    strips_->flush(frame);
    notes_->flush(frame);
}

void Lanes::drawAudio(const ui::GlFrame& frame, const ui::Rect& r, const model::Item& item,
                      const model::Take& take, const model::MediaSource& source,
                      std::uint32_t rgba) {
    auto pyramid = peaks_.get(source.hash, resolve_(source));
    if (!pyramid) {
        ++stats_.pending;  // a placeholder until the worker delivers (onReady repaints)
        return;
    }
    const int channels = std::max(1, pyramid->channels());
    const ui::Rect vis = r.intersect(frame.clip);
    if (vis.empty() || r.h < 4) return;
    const float px = 1.0f / frame.ratio;  // one device pixel in design units
    const double rate = pyramid->sampleRate();
    const double sourceSeconds = static_cast<double>(pyramid->frames()) / rate;
    const double framesPerUnit = rate * take.playrate / view_.pixelsPerSecond;
    const peaks::Level& level = pyramid->pick(framesPerUnit * static_cast<double>(px));
    const float gain = static_cast<float>(dbToGain(item.gainDb + take.gainDb));
    const float laneH = r.h / static_cast<float>(channels);
    const double itemStart = model::itemSpan(session_, item).startSec;
    for (int c = 0; c < channels; ++c) {
        const float mid = r.y + laneH * (static_cast<float>(c) + 0.5f), half = laneH * 0.5f - 1;
        strips_->beginRun();
        for (float x = std::floor(vis.x * frame.ratio) / frame.ratio; x < vis.right(); x += px) {
            // Item time → source time (stretch markers: P5.4 follow-up).
            double s0 = take.startOffset + (timeAt(x) - itemStart) * take.playrate;
            double s1 = s0 + static_cast<double>(px) / view_.pixelsPerSecond * take.playrate;
            if (item.loop && sourceSeconds > 0) {
                const double wrap = std::floor(s0 / sourceSeconds) * sourceSeconds;
                s0 -= wrap;
                s1 -= wrap;
            }
            const peaks::Span sp = pyramid->range(level, c, s0 * rate, s1 * rate);
            float top = mid - std::clamp(sp.max * gain, -1.0f, 1.0f) * half;
            float bottom = mid - std::clamp(sp.min * gain, -1.0f, 1.0f) * half;
            if (bottom - top < px) {  // silence still shows a hairline
                const float centre = (top + bottom) / 2;
                top = centre - px / 2;
                bottom = centre + px / 2;
            }
            strips_->pair(x, top, bottom, rgba);
            ++stats_.columns;
        }
        strips_->endRun();
    }
}

void Lanes::drawMidi(const ui::Rect& r, const model::Item& item, const model::Take& take,
                     const model::MidiSource& source, std::uint32_t rgba, std::uint32_t border) {
    const midi::Sequence& seq = source.sequence;
    if (seq.notes.empty() || r.h < 4) return;
    auto [lo, hi] = std::minmax_element(
        seq.notes.begin(), seq.notes.end(),
        [](const midi::Note& a, const midi::Note& b) { return a.pitch < b.pitch; });
    const int pitches = hi->pitch - lo->pitch + 3;  // a free row above and below
    const float noteH = std::max(1.0f, std::min(8.0f, r.h / static_cast<float>(pitches)));
    const float usedH = noteH * static_cast<float>(pitches);
    const float top = r.y + (r.h - usedH) / 2;
    const time::TimeMap& tm = *session_.timeMap;
    const model::ItemSpan span = model::itemSpan(session_, item);
    const double itemBeat = span.startBeat;
    const double ppq = seq.ppq > 0 ? seq.ppq : 960;
    for (const midi::Note& n : seq.notes) {
        const double b0 = static_cast<double>(n.start) / ppq - take.startOffset;
        const double b1 = b0 + static_cast<double>(n.length) / ppq;
        if (b1 <= 0) continue;
        const double s0 = tm.secondsAt(itemBeat + std::max(0.0, b0));
        if (s0 >= span.endSec) break;  // notes are sorted by start
        const double s1 = std::min(tm.secondsAt(itemBeat + b1), span.endSec);
        const float y = top + static_cast<float>(hi->pitch + 1 - n.pitch) * noteH;
        notes_->add({xAt(s0), y, std::max(1.0f, xAt(s1) - xAt(s0)), noteH}, rgba, border);
        ++stats_.notes;
    }
}

bool Lanes::onPointer(const ui::PointerEvent& e) {
    if (e.type != ui::PointerEvent::Type::Wheel) return false;
    Viewport v = view_;
    if (e.mods & ui::kCtrl) {  // zoom around the pointer
        const double at = timeAt(e.pos.x);
        v.pixelsPerSecond *= std::pow(1.25, static_cast<double>(e.wheelY));
        v.startSeconds = at - static_cast<double>(e.pos.x - bounds().x) / v.pixelsPerSecond;
    } else if (e.mods & ui::kShift || e.wheelX != 0) {
        const float dx = e.wheelX != 0 ? e.wheelX : e.wheelY;
        v.startSeconds -= static_cast<double>(dx) * 60.0 / view_.pixelsPerSecond;
    } else {
        v.scrollY -= e.wheelY * view_.rowHeight;
    }
    setViewport(v);
    return true;
}

}  // namespace supple::views
