#include "supple/views/arrange.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "supple/time/timeline.h"
#include "supple/ui/canvas.h"
#include "supple/ui/root.h"
#include "supple/ui/theme.h"

namespace supple::views {
namespace {

constexpr float kTcpWidth = 220;
constexpr float kBeatRowH = 22;    // bars | beats
constexpr float kTimeRowH = 15;    // m:ss.mmm
constexpr float kTempoRowH = 20;   // tempo and meter flags
constexpr float kRegionRowH = 15;  // regions
constexpr float kMarkerRowH = 16;  // markers
constexpr float kRulerHeight = kBeatRowH + kTimeRowH + kTempoRowH + kRegionRowH + kMarkerRowH;

constexpr float kMinBeatLabelPx = 62;
// The breathing space every pair of ruler labels keeps (design units).
constexpr float kLabelGap = 12;

double steadyMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// A stored position in its timebase as seconds (Timebase::Time is already
// seconds).
double atSeconds(const time::TimeMap& tm, double pos, model::Timebase tb) {
    return tb == model::Timebase::Time ? pos : tm.secondsAt(pos);
}

// m:ss (decimals 0) or m:ss.s (decimals 1): the format coarsens with the
// step, so a label always states exactly where its tick is.
std::string formatTime(double seconds, int decimals) {
    if (seconds < 0) seconds = 0;
    const auto totalMs = std::llround(seconds * 1000.0);
    const int minutes = static_cast<int>(totalMs / 60000);
    const int rest = static_cast<int>(totalMs % 60000);
    char buf[32];
    if (decimals <= 0)
        std::snprintf(buf, sizeof buf, "%d:%02d", minutes, rest / 1000);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d.%d", minutes, rest / 1000, (rest % 1000) / 100);
    return buf;
}

std::string formatBpm(double bpm) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.4g", bpm);
    return buf;
}

ui::Color parseColor(const std::string& text, ui::Color fallback) {
    if (text.size() != 7 || text[0] != '#') return fallback;
    char* end = nullptr;
    const auto rgb = std::strtoul(text.c_str() + 1, &end, 16);
    if (*end != '\0') return fallback;
    return ui::Color::hex(static_cast<std::uint32_t>(rgb));
}

// The finest nice time interval whose labels clear the previous one by the
// label's own width plus the gap at `pps`.
double timeStepFor(const ui::Canvas& c, double pps, float fontSize) {
    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};
    for (double s : steps) {
        const float w = c.textWidth(formatTime(0, s >= 1.0 ? 0 : 1), ui::Font::Mono, fontSize);
        if (s * pps >= static_cast<double>(w + kLabelGap)) return s;
    }
    return steps[sizeof(steps) / sizeof(steps[0]) - 1];
}

}  // namespace

// --- the ruler -------------------------------------------------------------------

class Ruler : public ui::Node {
  public:
    explicit Ruler(ArrangeView& view) : Node("ruler", ui::Role::Custom), view_(view) {}
    RulerStats stats;
    void paint(ui::Canvas& c) override {
        stats = {};
        const ui::Theme& t = c.theme();
        const model::Session& s = view_.session();
        const time::TimeMap& tm = *s.timeMap;
        const double pps = view_.viewport().pixelsPerSecond;
        const float x0 = bounds().x, x1 = bounds().right();
        const ui::Rect beatRow{x0, bounds().y, bounds().w, kBeatRowH};
        const ui::Rect timeRow{x0, bounds().y + kBeatRowH, bounds().w, kTimeRowH};
        const ui::Rect tempoRow{x0, timeRow.bottom(), bounds().w, kTempoRowH};
        const ui::Rect regionRow{x0, tempoRow.bottom(), bounds().w, kRegionRowH};
        const ui::Rect markRow{x0, regionRow.bottom(), bounds().w,
                               std::max(0.0f, bounds().bottom() - regionRow.bottom() - 1)};
        c.fillRect(bounds(), t.color("panel"));
        c.fillRect({x0, bounds().bottom() - 1, bounds().w, 1}, t.color("rule"));
        for (const ui::Rect& r : {beatRow, timeRow, tempoRow})
            c.fillRect({r.x, r.bottom() - 1, r.w, 1}, t.color("rule").withAlpha(0.6f));

        const ui::Color ink = t.color("ink");
        const ui::Color quiet = t.color("ink-quiet");
        const ui::Color faint = t.color("ink-faint");
        const double t0 = view_.timeAt(x0), t1 = view_.timeAt(x1);

        // The loop range shades the whole ruler, with a bar along the top.
        if (view_.loopEnabled() && view_.loopEnd() > view_.loopStart()) {
            const float lx0 = std::max(x0, view_.xAt(view_.loopStart()));
            const float lx1 = std::min(x1, view_.xAt(view_.loopEnd()));
            if (lx1 > lx0) {
                c.fillRect({lx0, bounds().y, lx1 - lx0, bounds().h},
                           t.color("accent").withAlpha(0.12f));
                c.fillRect({lx0, bounds().y, lx1 - lx0, 3}, t.color("accent"));
            }
        }

        // Bars and beats. Bar and beat labels share one row, so they share one
        // "last label" tracker: a label is placed only when the previous one's
        // width plus the gap has passed (never two labels touching).
        const float captionSize = t.type("caption");
        const double pxPerBeat = 60.0 / tm.bpmAt(tm.beatAt(t0)) * pps;
        const int subdivisions = pxPerBeat >= 110 ? 4 : pxPerBeat >= 52 ? 2 : 1;
        const bool beatLabels = pxPerBeat >= static_cast<double>(kMinBeatLabelPx);
        float lastLabelX = -1e9f, lastLabelW = 0;
        auto placeLabel = [&](float x, const std::string& text, ui::Color colour, bool isBar) {
            const float w = c.textWidth(text, ui::Font::Mono, captionSize);
            if (lastLabelX > -1e8f && x - lastLabelX < lastLabelW + kLabelGap) return;
            c.text({x + 3, beatRow.y, w + 4, kBeatRowH}, text, ui::Font::Mono, captionSize, colour,
                   ui::kLeft | ui::kMiddle);
            if (lastLabelX > -1e8f) {
                const float gap = x - lastLabelX;
                stats.minBarLabelGap = std::min(stats.minBarLabelGap, gap);
                stats.minBarSlack = std::min(stats.minBarSlack, gap - lastLabelW - kLabelGap);
            }
            ++(isBar ? stats.barLabels : stats.beatLabels);
            lastLabelX = x;
            lastLabelW = w;
        };
        const int firstBar = tm.barBeat(tm.beatAt(std::max(0.0, t0))).bar;
        int bar = std::max(1, firstBar);
        for (int guard = 0; guard < 20000; ++guard, ++bar) {
            const time::MeterChange meter = tm.meterAt(bar);
            const double barBeat = tm.beatOfBar(bar);
            const double beatQuarters = 4.0 / static_cast<double>(meter.denominator);
            const float barX = view_.xAt(tm.secondsAt(barBeat));
            if (barX > x1 + 80) break;
            if (barX >= x0 - 80) {
                const time::BarBeat bb = tm.barBeat(barBeat);
                placeLabel(barX, std::to_string(bb.bar) + "." + std::to_string(bb.beat), ink, true);
            }
            for (int b = 0; b < meter.numerator; ++b) {
                const double beat = barBeat + b * beatQuarters;
                const float x = view_.xAt(tm.secondsAt(beat));
                if (x < x0 - 1) continue;
                if (x > x1) break;
                const float th = b == 0 ? 10.0f : 6.0f;
                c.fillRect({x, beatRow.bottom() - th, 1, th}, b == 0 ? ink : quiet);
                if (beatLabels && b > 0) {
                    const time::BarBeat bb = tm.barBeat(beat);
                    placeLabel(x, std::to_string(bb.bar) + "." + std::to_string(bb.beat), quiet,
                               false);
                }
                for (int k = 1; k < subdivisions; ++k) {
                    const double sub = beat + beatQuarters * k / subdivisions;
                    const float sx = view_.xAt(tm.secondsAt(sub));
                    if (sx < x0 || sx > x1) continue;
                    c.fillRect({sx, beatRow.bottom() - 4, 1, 4}, faint.withAlpha(0.7f));
                    ++stats.subTicks;
                }
            }
        }

        // Time. The step coarsens as the zoom goes out and the format follows
        // it (m:ss from 1 s up, m:ss.s below), so no two labels can touch.
        const double step = timeStepFor(c, pps, captionSize);
        const int decimals = step >= 1.0 ? 0 : 1;
        float lastTimeX = -1e9f, lastTimeW = 0;
        for (double ts = std::floor(t0 / step) * step; ts <= t1; ts += step) {
            const float x = view_.xAt(ts);
            if (x < x0 - 1) continue;
            if (x > x1) break;
            const std::string label = formatTime(ts, decimals);
            const float w = c.textWidth(label, ui::Font::Mono, captionSize);
            if (lastTimeX > -1e8f && x - lastTimeX < lastTimeW + kLabelGap) continue;
            c.fillRect({x, timeRow.bottom() - 5, 1, 5}, quiet);
            c.text({x + 3, timeRow.y, w + 4, kTimeRowH}, label, ui::Font::Mono, captionSize, quiet,
                   ui::kLeft | ui::kMiddle);
            ++stats.timeLabels;
            if (lastTimeX > -1e8f) {
                const float gap = x - lastTimeX;
                stats.minTimeLabelGap = std::min(stats.minTimeLabelGap, gap);
                stats.minTimeSlack = std::min(stats.minTimeSlack, gap - lastTimeW - kLabelGap);
            }
            lastTimeX = x;
            lastTimeW = w;
        }

        // Tempo points and meter changes as flags.
        for (const time::TempoPoint& p : tm.tempo()) {
            const float x = view_.xAt(tm.secondsAt(p.beat));
            if (x < x0 - 1 || x > x1) continue;
            c.fillRect({x, tempoRow.y + 1, 1, kTempoRowH / 2.0f}, t.color("buss"));
            c.text({x + 3, tempoRow.y, 54, kTempoRowH / 2.0f}, formatBpm(p.bpm), ui::Font::Mono,
                   t.type("caption"), t.color("buss"), ui::kLeft | ui::kMiddle);
        }
        for (const time::MeterChange& m : tm.meter()) {
            const float x = view_.xAt(tm.secondsAt(tm.beatOfBar(m.bar)));
            if (x < x0 - 1 || x > x1) continue;
            const float y = tempoRow.y + kTempoRowH / 2.0f;
            c.fillRect({x, y, 1, kTempoRowH / 2.0f - 1}, t.color("master"));
            c.text({x + 3, y, 54, kTempoRowH / 2.0f},
                   std::to_string(m.numerator) + "/" + std::to_string(m.denominator),
                   ui::Font::Mono, t.type("caption"), t.color("master"), ui::kLeft | ui::kMiddle);
        }

        // Regions on their own row, markers below it.
        for (const auto& e : s.regions) {
            const model::Region& rg = *e.value;
            const double a = atSeconds(tm, rg.start, rg.timebase);
            const double b = atSeconds(tm, rg.end, rg.timebase);
            const float rx0 = std::max(x0, view_.xAt(a)), rx1 = std::min(x1, view_.xAt(b));
            if (rx1 <= rx0) continue;
            const ui::Color col = parseColor(rg.color, t.color("selection"));
            c.fillRect({rx0, regionRow.y, rx1 - rx0, regionRow.h}, col.withAlpha(0.35f));
            c.fillRect({rx0, regionRow.bottom() - 2, rx1 - rx0, 2}, col);
            if (rx1 - rx0 > 24)
                c.text({rx0 + 4, regionRow.y, rx1 - rx0 - 6, regionRow.h}, rg.name, ui::Font::Ui,
                       t.type("caption"), ink, ui::kLeft | ui::kMiddle);
        }
        for (const auto& e : s.markers) {
            const model::Marker& mk = *e.value;
            const float x = view_.xAt(atSeconds(tm, mk.pos, mk.timebase));
            if (x < x0 - 1 || x > x1) continue;
            const ui::Color col = parseColor(mk.color, t.color("accent"));
            c.fillRect({x, markRow.y + 1, 1, markRow.h - 2}, col);
            c.fillRect({x, markRow.y + 1, 6, 6}, col);
            c.text({x + 8, markRow.y, 90, markRow.h}, mk.name, ui::Font::Ui, t.type("caption"),
                   quiet, ui::kLeft | ui::kMiddle);
        }
    }

  private:
    ArrangeView& view_;
};

// --- the track control panel (placeholder: name and type badge) ---------------------

class TcpHeader : public ui::Node {
  public:
    explicit TcpHeader(ArrangeView& view) : Node("tcp-header"), view_(view) {}
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
        c.text({bounds().x + 12, bounds().y, bounds().w - 20, bounds().h},
               "Tracks  " + std::to_string(view_.trackLayout().rowCount()), ui::Font::UiBold,
               c.theme().type("caption"), c.theme().color("ink-quiet"), ui::kLeft | ui::kMiddle);
    }

  private:
    ArrangeView& view_;
};

class Corner : public ui::Node {
  public:
    using Node::Node;
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
    }
};

// --- the playhead (on the overlay tree) ---------------------------------------------

class Playhead : public ui::Node {
  public:
    explicit Playhead(ArrangeView& view) : Node("playhead"), view_(view) {}
    // The overlay's children are arranged with their own bounds, so take the
    // parent's (the full window).
    void arrange(const ui::Canvas&, const ui::Rect&) override {
        if (parent()) setBounds(parent()->bounds());
    }
    void paint(ui::Canvas& c) override {
        const ui::Rect a = view_.lanesRect();
        if (a.empty()) return;
        const float x = view_.playheadX();
        if (x < a.x - 1 || x > a.right() + 1) return;
        const ui::Color col = c.theme().color("accent");
        c.line({x, a.y}, {x, a.bottom()}, col, 1.0f);
        c.fillRect({x - 3, a.y, 7, 7}, col);
    }

  private:
    ArrangeView& view_;
};

// --- the view ----------------------------------------------------------------------

ArrangeView::ArrangeView(peaks::PeakService& peaks, Resolve resolve, TransportSource transport)
    : Node("arrange", ui::Role::Custom),
      peaks_(peaks),
      resolve_(std::move(resolve)),
      transportSource_(std::move(transport)) {
    gestures_ = std::make_unique<ItemGestures>(*this);
    interactive = true;
    focusable = true;
    layout.dir = ui::Layout::Dir::Column;

    auto& header = emplace<Node>("arrange-header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(kRulerHeight);
    header_ = &header.emplace<TcpHeader>(*this);
    header_->layout.width = ui::SizeRule::fixed(kTcpWidth);
    ruler_ = &header.emplace<Ruler>(*this);
    ruler_->layout.width = ui::SizeRule::fraction();
    auto& corner = header.emplace<Corner>("arrange-corner");
    corner.layout.width = ui::SizeRule::fixed(ui::ScrollBar::kThickness);

    auto& body = emplace<Node>("arrange-body");
    body.layout.dir = ui::Layout::Dir::Row;
    // The panel's callbacks forward to the view's, so the app can install them
    // after the tree is built.
    TcpCallbacks forward;
    forward.selected = [this](const model::Id& id) {
        return tcpCallbacks_.selected && tcpCallbacks_.selected(id);
    };
    forward.select = [this](const model::Id& id, unsigned mods) {
        if (tcpCallbacks_.select) tcpCallbacks_.select(id, mods);
    };
    forward.commit = [this](std::string_view command, json::Value args, std::string gesture) {
        if (tcpCallbacks_.commit)
            tcpCallbacks_.commit(command, std::move(args), std::move(gesture));
    };
    forward.addStrip = [this](std::string_view type) {
        if (tcpCallbacks_.addStrip) tcpCallbacks_.addStrip(type);
    };
    forward.meterShown = [this](const std::string& track, ui::Meter* meter) {
        if (tcpCallbacks_.meterShown) tcpCallbacks_.meterShown(track, meter);
    };
    forward.meterHidden = [this](ui::Meter* meter) {
        if (tcpCallbacks_.meterHidden) tcpCallbacks_.meterHidden(meter);
    };
    // The input button's two: what to write on it, and the picker (both are the
    // app's, since it owns the engine's input list and the popup).
    forward.inputText = [this](const model::Id& id, const std::string& channels) {
        return tcpCallbacks_.inputText ? tcpCallbacks_.inputText(id, channels) : std::string{};
    };
    forward.pickInput = [this](const model::Id& id, const ui::Rect& anchor) {
        if (tcpCallbacks_.pickInput) tcpCallbacks_.pickInput(id, anchor);
    };
    tcp_ = &body.emplace<TcpPanel>(layout_, view_, std::move(forward));
    tcp_->layout.width = ui::SizeRule::fixed(kTcpWidth);
    auto& lanesCol = body.emplace<Node>("lanes-column");
    lanesCol.layout.dir = ui::Layout::Dir::Column;
    lanesCol.layout.width = ui::SizeRule::fraction();
    lanes_ = &lanesCol.emplace<Lanes>(peaks_, resolve_, layout_, view_);
    lanes_->layout.height = ui::SizeRule::fraction();
    hbar_ = &lanesCol.emplace<ui::ScrollBar>("hscroll", ui::ScrollBar::Axis::Horizontal);
    hbar_->layout.height = ui::SizeRule::fixed(ui::ScrollBar::kThickness);
    hbar_->onScroll = [this](float v) {
        Viewport nv = view_;
        nv.startSeconds = static_cast<double>(v);
        setViewport(nv);
    };
    vbar_ = &body.emplace<ui::ScrollBar>("vscroll", ui::ScrollBar::Axis::Vertical);
    vbar_->layout.width = ui::SizeRule::fixed(ui::ScrollBar::kThickness);
    vbar_->onScroll = [this](float v) {
        Viewport nv = view_;
        nv.scrollY = v;
        setViewport(nv);
    };
    // Any row-geometry change (a new session, per-track heights, vertical zoom)
    // re-clamps the scroll and refreshes both bars.
    layout_.onChanged = [this] {
        clampScroll();
        syncScrollbars();
        if (tcp_) tcp_->invalidate();
        damageViews();
    };
}

ArrangeView::~ArrangeView() = default;

void ArrangeView::show(const model::Session& session) {
    if (!layout_.show(session)) return;
    lanes_->rebuild();
    tcp_->setSession(session);
    header_->invalidate();
    clampScroll();
    syncScrollbars();
    damageViews();
}

bool ArrangeView::poll() {
    if (!transportSource_) return false;
    const double now = steadyMs();
    if (now - lastPollMs_ < 15.0) return false;
    lastPollMs_ = now;
    const Transport t = transportSource_();
    const bool wasPlaying = transport_.playing;
    transport_ = t;
    if (t.seconds != playhead_) {
        setPlayhead(t.seconds);
        return true;
    }
    return t.playing != wasPlaying;
}

void ArrangeView::setPlayhead(double seconds) {
    if (seconds == playhead_) return;
    playhead_ = seconds;
    applyFollow();
    if (ui::Root* r = root()) r->requestAnimationFrame();
}

void ArrangeView::setLoopRange(double start, double end, bool on) {
    if (transport_.loopStart == start && transport_.loopEnd == end && transport_.loopEnabled == on)
        return;
    transport_.loopEnabled = on;
    transport_.loopStart = start;
    transport_.loopEnd = end;
    if (ruler_) ruler_->invalidate();
}

void ArrangeView::setViewport(const Viewport& v) {
    view_ = v;
    view_.pixelsPerSecond = std::clamp(view_.pixelsPerSecond, 0.01, 200000.0);
    view_.startSeconds = std::max(0.0, view_.startSeconds);
    clampScroll();
    syncScrollbars();
    damageViews();
}

void ArrangeView::syncScrollbars() {
    if (!hbar_ || !vbar_) return;
    hbar_->setRange(static_cast<float>(totalSeconds()), static_cast<float>(viewWidthSeconds()));
    hbar_->setValue(static_cast<float>(view_.startSeconds));
    const float shown = lanes_ ? lanes_->bounds().h : 0.0f;
    vbar_->setRange(layout_.contentHeight(), shown);
    vbar_->setValue(view_.scrollY);
}

void ArrangeView::clampScroll() {
    const float visibleHeight = lanes_ ? lanes_->bounds().h : 0.0f;
    if (visibleHeight <= 0) return;  // not laid out yet: keep what was asked for
    const float maxScroll = std::max(0.0f, layout_.contentHeight() - visibleHeight);
    view_.scrollY = std::clamp(view_.scrollY, 0.0f, maxScroll);
}

void ArrangeView::arrange(const ui::Canvas& canvas, const ui::Rect& r) {
    // Clamp before arranging: the rows the TCP binds must match the scroll the
    // frame will actually paint with (the visible height is known from `r`).
    const float visibleHeight = std::max(0.0f, r.h - kRulerHeight - ui::ScrollBar::kThickness);
    view_.scrollY =
        std::clamp(view_.scrollY, 0.0f, std::max(0.0f, layout_.contentHeight() - visibleHeight));
    ui::Node::arrange(canvas, r);
    clampScroll();  // exact, now that the children have their bounds
    syncScrollbars();
}

double ArrangeView::viewWidthSeconds() const {
    const double pps = view_.pixelsPerSecond > 0 ? view_.pixelsPerSecond : 40;
    return static_cast<double>(lanes_ ? lanes_->bounds().w : 0.0f) / pps;
}

double ArrangeView::totalSeconds() const {
    double end = 0;
    const model::Session& s = layout_.session();
    for (const auto& e : s.items) end = std::max(end, model::itemSpan(s, *e.value).endSec);
    for (const auto& e : s.regions)
        end = std::max(end, atSeconds(*s.timeMap, e.value->end, e.value->timebase));
    for (const auto& e : s.markers)
        end = std::max(end, atSeconds(*s.timeMap, e.value->pos, e.value->timebase));
    if (transport_.loopEnabled) end = std::max(end, transport_.loopEnd);
    return end + 8.0;  // margin past the session
}

void ArrangeView::damageViews() {
    ui::Rect r;
    for (ui::Node* n : {static_cast<ui::Node*>(lanes_), static_cast<ui::Node*>(ruler_),
                        static_cast<ui::Node*>(hbar_), static_cast<ui::Node*>(vbar_)})
        if (n) r = r.unite(n->bounds());
    if (!r.empty()) invalidate(r);
}

void ArrangeView::applyFollow() {
    if (!lanes_ || follow_ == Follow::Off) return;
    const ui::Rect a = lanes_->bounds();
    if (a.w <= 0) return;
    const float x = playheadX();
    Viewport v = view_;
    if (follow_ == Follow::Continuous) {
        const double half = static_cast<double>(a.w) / 2.0 / v.pixelsPerSecond;
        const double target = std::max(0.0, playhead_ - half);
        if (std::abs(target - v.startSeconds) < 1e-9) return;
        v.startSeconds = target;
    } else {  // page: jump a page when the playhead leaves the view
        if (x <= a.right() - 4 && x >= a.x) return;
        v.startSeconds =
            std::max(0.0, playhead_ - static_cast<double>(a.w) * 0.25 / v.pixelsPerSecond);
    }
    setViewport(v);
}

bool ArrangeView::installOverlay(ui::Root& root) {
    root.overlay().add(std::make_unique<Playhead>(*this));
    gestures_->installOverlay(root);
    return true;
}

ui::Node& ArrangeView::tcp() { return *tcp_; }
ui::Node& ArrangeView::ruler() { return *ruler_; }
const RulerStats& ArrangeView::rulerStats() const { return ruler_->stats; }

ui::Rect ArrangeView::rowRect(std::size_t row) const {
    return lanes_ ? lanes_->rowRect(row) : ui::Rect{};
}

ui::Rect ArrangeView::tcpRowRect(std::size_t row) const {
    return tcp_ ? tcp_->rowRect(row) : ui::Rect{};
}

bool ArrangeView::onPointer(const ui::PointerEvent& e) {
    if (e.type != ui::PointerEvent::Type::Wheel) return gestures_->pointer(e);
    Viewport v = view_;
    const bool ctrl = (e.mods & ui::kCtrl) != 0, shift = (e.mods & ui::kShift) != 0;
    if (ctrl && shift) {  // vertical zoom: the global row height
        layout_.setGlobalHeight(static_cast<float>(static_cast<double>(layout_.globalHeight()) *
                                                   std::pow(1.25, static_cast<double>(e.wheelY))));
        return true;
    }
    if (ctrl) {  // horizontal zoom around the pointer
        const double at = timeAt(e.pos.x);
        v.pixelsPerSecond *= std::pow(1.25, static_cast<double>(e.wheelY));
        v.pixelsPerSecond = std::clamp(v.pixelsPerSecond, 0.01, 200000.0);
        v.startSeconds = at - static_cast<double>(e.pos.x - lanes_->bounds().x) / v.pixelsPerSecond;
        v.startSeconds = std::max(0.0, v.startSeconds);
    } else if (shift || e.wheelX != 0) {  // horizontal scroll
        const float dx = e.wheelX != 0 ? e.wheelX : e.wheelY;
        v.startSeconds =
            std::max(0.0, v.startSeconds - static_cast<double>(dx) * 60.0 / v.pixelsPerSecond);
    } else {  // vertical scroll
        v.scrollY -= e.wheelY * layout_.globalHeight();
    }
    setViewport(v);
    return true;
}

// Only the view's own zoom; Home/End belong to the keymap
// (transport.seek:start/end), which the app routes through Root::onUnhandledKey.
bool ArrangeView::onKey(const ui::KeyEvent& e) {
    const double zoom = 1.25;
    switch (e.key) {
        case SDLK_PLUS:
        case SDLK_EQUALS:
        case SDLK_KP_PLUS: {
            Viewport v = view_;
            const double keep = playhead_ - v.startSeconds;
            v.pixelsPerSecond = std::clamp(v.pixelsPerSecond * zoom, 0.01, 200000.0);
            v.startSeconds =
                std::max(0.0, playhead_ - keep * (view_.pixelsPerSecond / v.pixelsPerSecond));
            setViewport(v);
            return true;
        }
        case SDLK_MINUS:
        case SDLK_KP_MINUS: {
            Viewport v = view_;
            const double keep = playhead_ - v.startSeconds;
            v.pixelsPerSecond = std::clamp(v.pixelsPerSecond / zoom, 0.01, 200000.0);
            v.startSeconds =
                std::max(0.0, playhead_ - keep * (view_.pixelsPerSecond / v.pixelsPerSecond));
            setViewport(v);
            return true;
        }
        default:
            return false;
    }
}

std::string_view followName(Follow f) {
    switch (f) {
        case Follow::Off:
            return "off";
        case Follow::Page:
            return "page";
        case Follow::Continuous:
            return "continuous";
    }
    return "page";
}

std::optional<Follow> followFrom(std::string_view name) {
    if (name == "off") return Follow::Off;
    if (name == "page") return Follow::Page;
    if (name == "continuous") return Follow::Continuous;
    return std::nullopt;
}

}  // namespace supple::views
