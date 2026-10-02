#include "transport_bar.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "supple/time/timeline.h"
#include "supple/ui/canvas.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/theme.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string timeText(double seconds) {
    if (seconds < 0) seconds = 0;
    const auto totalMs = std::llround(seconds * 1000.0);
    const int minutes = static_cast<int>(totalMs / 60000);
    const int rest = static_cast<int>(totalMs % 60000);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d.%03d", minutes, rest / 1000, rest % 1000);
    return buf;
}

std::string barsText(double beat, const time::TimeMap& tm) {
    const time::BarBeat bb = tm.barBeat(beat);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%d.%d.%03d", bb.bar, bb.beat,
                  static_cast<int>(bb.fraction * 1000.0));
    return buf;
}

std::string bpmText(const time::TimeMap& tm) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.4g", tm.bpmAt(0));
    return buf;
}

std::string meterText(const time::TimeMap& tm) {
    const time::MeterChange m = tm.meterAt(1);
    return std::to_string(m.numerator) + "/" + std::to_string(m.denominator);
}

// Setting a label's text damages it (and relays out), so only write when the
// value actually changed: the idle loop must stay at zero work.
void setIfChanged(ui::Label* label, const std::string& text) {
    if (label && label->text() != text) label->setText(text);
}

void setIfChanged(ui::Button* button, const std::string& text) {
    if (button && button->valueText() != text) button->setText(text);
}

}  // namespace

TransportBar::TransportBar(engine::EngineApp& app, AppActions& actions, cmd::Dispatcher& dispatcher,
                           ui::Root& root)
    : app_(app), actions_(actions), dispatcher_(dispatcher), root_(root) {}

std::unique_ptr<ui::Node> TransportBar::build() {
    auto bar = std::make_unique<ui::Node>("transport");
    bar->layout.dir = ui::Layout::Dir::Row;
    bar->layout.height = ui::SizeRule::fixed(44);
    bar->layout.padding = ui::Insets::xy(12, 0);
    bar->layout.gap = 6;
    auto actionButton = [&](const char* id, const char* text, const char* action, float width) {
        auto& b = bar->emplace<ui::Button>(id, text, [this, action] {
            auto r = actions_.run(action);
            if (!r && actions_.onError) actions_.onError(r.error().message);
        });
        b.layout.width = ui::SizeRule::fixed(width);
        b.layout.height = ui::SizeRule::fixed(24);
        return &b;
    };
    actionButton("transport.start", "start", "transport.seek:start", 44);  // also W, Home
    play_ = actionButton("transport.play", "play", "transport.toggle", 44);
    record_ = actionButton("transport.record", "rec", "transport.record", 38);
    loop_ = actionButton("transport.loop", "loop", "transport.loop.toggle", 48);
    punch_ = &bar->emplace<ui::Button>("transport.punch", "punch", [this] {
        // No action: the punch range is set elsewhere (P5.6); the button only
        // toggles it.
        json::Value args = json::Value::object();
        args["enabled"] = !app_.punch().enabled;
        auto r = dispatcher_.execute({"transport.punch", std::move(args), kOrigin});
        if (!r && actions_.onError) actions_.onError(r.error().message);
    });
    punch_->layout.width = ui::SizeRule::fixed(52);
    punch_->layout.height = ui::SizeRule::fixed(24);
    click_ = &bar->emplace<ui::Button>("transport.metronome", "click", [this] {
        json::Value args = json::Value::object();
        args["enabled"] = !app_.metronome().enabled;
        auto r = dispatcher_.execute({"transport.metronome", std::move(args), kOrigin});
        if (!r && actions_.onError) actions_.onError(r.error().message);
        sync();
    });
    click_->layout.width = ui::SizeRule::fixed(56);
    click_->layout.height = ui::SizeRule::fixed(24);
    bar->emplace<ui::Node>("transport.gap").layout.width = ui::SizeRule::fixed(10);
    position_ = &bar->emplace<ui::Label>("transport.position", "1.1.000", ui::Font::MonoBold,
                                         "title", "ink");
    time_ =
        &bar->emplace<ui::Label>("transport.time", "0:00.000", ui::Font::Mono, "mono", "ink-quiet");
    tempo_ = &bar->emplace<ui::Button>("transport.tempo", "120", [this] { openTempoForm(); });
    tempo_->layout.width = ui::SizeRule::fixed(58);
    tempo_->layout.height = ui::SizeRule::fixed(22);
    meter_ = &bar->emplace<ui::Button>("transport.meter", "4/4", [this] { openMeterForm(); });
    meter_->layout.width = ui::SizeRule::fixed(48);
    meter_->layout.height = ui::SizeRule::fixed(22);
    bar->emplace<ui::Node>("transport.spacer");  // takes the rest
    engine_ =
        &bar->emplace<ui::Label>("transport.engine", "", ui::Font::Mono, "small", "ink-quiet");
    return bar;
}

void TransportBar::sync() {
    const model::Session& s = app_.head();
    engine::TransportInfo info;
    if (auto r = app_.transport("get")) info = *r;
    if (position_) setIfChanged(position_, barsText(info.beat, *s.timeMap));
    if (time_) setIfChanged(time_, timeText(info.seconds));
    if (tempo_) setIfChanged(tempo_, bpmText(*s.timeMap));
    if (meter_) setIfChanged(meter_, meterText(*s.timeMap));
    if (play_) setIfChanged(play_, info.playing ? "stop" : "play");
    if (loop_) setIfChanged(loop_, info.loop.enabled ? "loop on" : "loop");
    if (punch_) setIfChanged(punch_, info.punch.enabled ? "punch on" : "punch");
    if (click_) setIfChanged(click_, app_.metronome().enabled ? "click on" : "click");
    const double now = nowMs();
    if (engine_ && now - lastStatusMs_ >= 250.0) {  // ≤ 4 Hz
        lastStatusMs_ = now;
        const engine::EngineStatus st = app_.engineStatus();
        if (!st.running) {
            setIfChanged(engine_, "engine stopped");
        } else {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%.1fk/%lld  xr %lld  err %lld  mis %lld",
                          st.sampleRate / 1000.0, static_cast<long long>(st.block),
                          static_cast<long long>(st.xruns),
                          static_cast<long long>(st.processErrors),
                          static_cast<long long>(st.anticipationMisses));
            setIfChanged(engine_, buf);
        }
    }
}

void TransportBar::openTempoForm() {
    auto form = std::make_unique<ui::Node>("tempo.form");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 8;
    form->layout.width = ui::SizeRule::fixed(240);
    form->emplace<ui::Label>("tempo.title", "Tempo (BPM)", ui::Font::UiBold, "title");
    auto& field = form->emplace<ui::TextField>("tempo.field", bpmText(*app_.head().timeMap));
    field.layout.height = ui::SizeRule::fixed(24);
    auto apply = [this, &field] {
        const double bpm = std::atof(field.text().c_str());
        if (!(bpm >= 1.0 && bpm <= 1000.0)) {
            if (actions_.onError) actions_.onError("tempo: 1..1000 BPM");
            return;
        }
        // Keep the map; the session tempo is the first point.
        json::Value points = json::Value::array();
        bool first = true;
        for (const time::TempoPoint& p : app_.head().timeMap->tempo()) {
            json::Value point = json::Value::object();
            point["beat"] = p.beat;
            point["bpm"] = first ? bpm : p.bpm;
            point["ramp"] = p.shape == time::TempoShape::Ramp;
            points.push_back(std::move(point));
            first = false;
        }
        json::Value args = json::Value::object();
        args["points"] = std::move(points);
        auto r = dispatcher_.execute({"tempo.set", std::move(args), kOrigin});
        if (!r && actions_.onError)
            actions_.onError(r.error().message);
        else
            root_.closePopup();
    };
    field.onSubmit = [apply](const std::string&) { apply(); };
    auto& row = form->emplace<ui::Node>("tempo.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(26);
    row.layout.gap = 6;
    row.emplace<ui::Button>("tempo.set", "Set", apply);
    row.emplace<ui::Button>("tempo.cancel", "Cancel", [this] { root_.closePopup(); });
    auto& content = root_.openPopup(std::move(form), {}, ui::Placement::Center);
    if (ui::Node* f = content.find("tempo.field")) root_.focus(f);
}

void TransportBar::openMeterForm() {
    auto form = std::make_unique<ui::Node>("meter.form");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 8;
    form->layout.width = ui::SizeRule::fixed(240);
    form->emplace<ui::Label>("meter.title", "Meter from bar 1 (N/D)", ui::Font::UiBold, "title");
    auto& field = form->emplace<ui::TextField>("meter.field", meterText(*app_.head().timeMap));
    field.layout.height = ui::SizeRule::fixed(24);
    auto apply = [this, &field] {
        json::Value args = json::Value::object();
        args["bar"] = 1;
        args["meter"] = field.text();
        auto r = dispatcher_.execute({"timesig.set", std::move(args), kOrigin});
        if (!r && actions_.onError)
            actions_.onError(r.error().message);
        else
            root_.closePopup();
    };
    field.onSubmit = [apply](const std::string&) { apply(); };
    auto& row = form->emplace<ui::Node>("meter.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(26);
    row.layout.gap = 6;
    row.emplace<ui::Button>("meter.set", "Set", apply);
    row.emplace<ui::Button>("meter.cancel", "Cancel", [this] { root_.closePopup(); });
    auto& content = root_.openPopup(std::move(form), {}, ui::Placement::Center);
    if (ui::Node* f = content.find("meter.field")) root_.focus(f);
}

}  // namespace supple::gui
