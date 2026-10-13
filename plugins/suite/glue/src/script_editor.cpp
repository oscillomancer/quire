// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "supple/suite/script_editor.h"

#include <Theme.h>  // the product's palette: product::kTheme

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "core/Param.h"
#include "gui/Plot.h"
#include "gui/Widgets.h"
#include "supple/script.h"
#include "supple/script_runtime.h"

namespace supple::suite {
namespace {

namespace S = supple::script;
namespace D = supple::script::detail;

// Loop iterations an editor hook may take: an audio hook's.
constexpr std::int64_t kBudget = std::int64_t{1} << 26;
// Text a hook may make (format(), Param::text()) before it runs out.
constexpr int kTextRoom = 32 * 1024;
constexpr double kDefaultWidth = 480.0, kDefaultHeight = 320.0;

// A parameter's value as the DSP is using it: the host's with its
// modulation, or what the processor last reported for a meter().
double heard(const gui::EditorHost& host, const ParamDef& def) {
    if (def.isOutput()) return toPlain(def, host.value(def.id));
    return toPlain(def, sanitizeHost(def, host.value(def.id) + host.modulation(def.id)));
}

// The editor's copy of the plugin: the struct, its runtime record, and what
// editor code reaches through that record (text, parameter names).
class Copy final : public D::EditorServices {
  public:
    Copy(std::shared_ptr<const D::Code> code, const ParamTable& table, double sampleRate)
        : code_(std::move(code)), table_(table), self_(code_->create()), text_(kTextRoom) {
        record_.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        record_.editor = this;
        D::Instance::bind(*static_cast<S::Plugin*>(self_), &record_);
        for (const D::ParamModel& m : code_->params()) {
            const int i = table.indexOf(m.id);
            if (i < 0) continue;
            auto* p = reinterpret_cast<S::Param*>(static_cast<unsigned char*>(self_) + m.offset);
            D::ParamBinding::setSlot(*p, i);
            const ParamDef& def = table.at(static_cast<size_t>(i));
            D::ParamBinding::setRange(*p, def.min, def.is(pflag::kOffAtMin));
            params_.push_back({p, static_cast<size_t>(i)});
        }
    }
    ~Copy() { code_->destroy(self_); }
    Copy(const Copy&) = delete;
    Copy& operator=(const Copy&) = delete;

    const D::Code& code() const { return *code_; }
    void* self() const { return self_; }
    const D::Status& status() const { return record_.status; }

    // Every Param to the value the DSP is using.
    void listen(const gui::EditorHost& host) {
        for (const auto& [param, index] : params_)
            D::ParamBinding::setValue(*param, heard(host, table_.at(index)), false);
    }

    // A hook, in the copy, on its budget. False when it was stopped.
    bool run(D::Code::UiHook hook, void* object) {
        used_ = 0;
        D::CurrentScope scope(&record_);
        record_.budget = kBudget;
        const std::uint32_t before = record_.status.budgetExceeded.load(std::memory_order_relaxed);
        hook(&record_, self_, object);
        return record_.status.budgetExceeded.load(std::memory_order_relaxed) == before;
    }

    // --- D::EditorServices ---
    char* text(int bytes) override {
        if (bytes <= 0 || used_ + bytes > kTextRoom) return nullptr;
        char* at = text_.data() + used_;
        used_ += bytes;
        return at;
    }
    const char* paramText(int slot, double value) override {
        if (slot < 0 || static_cast<size_t>(slot) >= table_.count()) return "";
        const ParamDef& def = table_.at(static_cast<size_t>(slot));
        char* out = text(64);
        if (!out) return "";
        formatValue(def, toHost(def, value), out, 64);
        return out;
    }
    const char* paramName(int slot) override {
        if (slot < 0 || static_cast<size_t>(slot) >= table_.count()) return "";
        return table_.at(static_cast<size_t>(slot)).name;
    }
    double paramNormalized(int slot, double value) override {
        if (slot < 0 || static_cast<size_t>(slot) >= table_.count()) return 0.0;
        const ParamDef& def = table_.at(static_cast<size_t>(slot));
        return toNormalized(def, sanitizeHost(def, toHost(def, value)));
    }
    void editParam(int slot, Edit kind, double value) override {
        if (!editing_ || slot < 0 || static_cast<size_t>(slot) >= table_.count()) return;
        const auto index = static_cast<size_t>(slot);
        const ParamDef& def = table_.at(index);
        if (def.isOutput() || (kind != Edit::Default && !std::isfinite(value))) return;
        const double host = sanitizeHost(def, kind == Edit::Value      ? toHost(def, value)
                                              : kind == Edit::Position ? fromNormalized(def, value)
                                                                       : hostDefault(def));
        for (const auto& [param, i] : params_)
            if (i == index) D::ParamBinding::setValue(*param, toPlain(def, host), false);
        for (Edited& e : edits_)
            if (e.index == index) {
                e.host = host;
                return;
            }
        edits_.push_back({index, host});
    }

    // pointer()'s edits, taken while allowed: each parameter's last.
    struct Edited {
        size_t index;  // in the table
        double host;
    };
    void allowEdits(bool on) { editing_ = on; }
    std::vector<Edited> takeEdits() { return std::exchange(edits_, {}); }

  private:
    std::shared_ptr<const D::Code> code_;
    const ParamTable& table_;
    void* self_;
    D::Instance record_;
    std::vector<std::pair<S::Param*, size_t>> params_;  // each Param member, and its table index
    std::vector<char> text_;
    int used_ = 0;
    bool editing_ = false;
    std::vector<Edited> edits_;
};

// What editor() asks for: a size, and controls in the order given.
class Recorder final : public D::EditorBuilder {
  public:
    struct Item {
        enum class Kind { Control, XyPad, Envelope, File, Keyboard, Meters } kind = Kind::Control;
        Control control = Control::Knob;
        int slots[4] = {-1, -1, -1, -1};
        const void* member = nullptr;
        int low = 48, high = 72;
        gui::Rect at;
    };
    double width = kDefaultWidth, height = kDefaultHeight;
    std::vector<Item> items;

    void size(double w, double h) override {
        if (std::isfinite(w) && std::isfinite(h)) {
            width = std::clamp(w, 120.0, 2400.0);
            height = std::clamp(h, 80.0, 1600.0);
        }
    }
    void control(Control kind, int slot, double x, double y, double w, double h) override {
        Item i;
        i.control = kind;
        i.slots[0] = slot;
        add(i, x, y, w, h);
    }
    void xyPad(int across, int up, double x, double y, double w, double h) override {
        Item i;
        i.kind = Item::Kind::XyPad;
        i.slots[0] = across;
        i.slots[1] = up;
        add(i, x, y, w, h);
    }
    void envelope(int attack, int decay, int sustain, int release, double x, double y, double w,
                  double h) override {
        Item i;
        i.kind = Item::Kind::Envelope;
        i.slots[0] = attack;
        i.slots[1] = decay;
        i.slots[2] = sustain;
        i.slots[3] = release;
        add(i, x, y, w, h);
    }
    void file(const void* member, double x, double y, double w, double h) override {
        Item i;
        i.kind = Item::Kind::File;
        i.member = member;
        add(i, x, y, w, h);
    }
    void keyboard(int lowKey, int highKey, double x, double y, double w, double h) override {
        Item i;
        i.kind = Item::Kind::Keyboard;
        i.low = std::clamp(std::min(lowKey, highKey), 0, 127);
        i.high = std::clamp(std::max(lowKey, highKey), 0, 127);
        add(i, x, y, w, h);
    }
    void meters(double x, double y, double w, double h) override {
        Item i;
        i.kind = Item::Kind::Meters;
        add(i, x, y, w, h);
    }

  private:
    void add(Item i, double x, double y, double w, double h) {
        if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h)) ||
            w <= 0.0 || h <= 0.0)
            return;
        auto clamp = [](double v) { return static_cast<float>(std::clamp(v, -10000.0, 10000.0)); };
        i.at = {clamp(x), clamp(y), clamp(w), clamp(h)};
        items.push_back(i);
    }
};

// A Canvas's painter: script.h's canvas calls as NanoVG's.
class Painter final : public D::Painter {
  public:
    Painter(NVGcontext* vg, const gui::Theme& theme, double width, double height)
        : vg_(vg), theme_(theme), width_(width), height_(height) {
        nvgSave(vg_);
        nvgFillColor(vg_, colour(S::Theme::text));
        nvgStrokeColor(vg_, colour(S::Theme::text));
        nvgStrokeWidth(vg_, 1.0f);
        nvgFontFace(vg_, gui::font::kUi);
        nvgFontSize(vg_, 13.0f);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    }
    // Whatever save()s the script left open, closed.
    ~Painter() {
        for (; depth_ > 0; --depth_) nvgRestore(vg_);
        nvgRestore(vg_);
    }
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    NVGcolor colour(int c) const {
        if (c >= S::Theme::ground && c <= S::Theme::alert) {
            const std::uint32_t palette[] = {
                theme_.ground,  theme_.surface,    theme_.raised,    theme_.recess, theme_.line,
                theme_.lineLit, theme_.textFaint,  theme_.textMuted, theme_.text,   theme_.accent,
                theme_.accent2, theme_.modulation, theme_.alert};
            return gui::rgb(palette[c - S::Theme::ground]);
        }
        return gui::rgb(static_cast<std::uint32_t>(c) & 0xFFFFFFu);
    }

    double width() const override { return width_; }
    double height() const override { return height_; }
    void save() override {
        if (depth_ >= 24) return;  // NanoVG keeps 32 states; some are the editor's
        ++depth_;
        nvgSave(vg_);
    }
    void restore() override {
        if (depth_ == 0) return;
        --depth_;
        nvgRestore(vg_);
    }
    void translate(double x, double y) override { nvgTranslate(vg_, f(x), f(y)); }
    void rotate(double radians) override { nvgRotate(vg_, f(radians)); }
    void scale(double x, double y) override { nvgScale(vg_, f(x), f(y)); }
    void fillStyle(int c) override { nvgFillColor(vg_, colour(c)); }
    void strokeStyle(int c) override { nvgStrokeColor(vg_, colour(c)); }
    void globalAlpha(double alpha) override { nvgGlobalAlpha(vg_, f(std::clamp(alpha, 0.0, 1.0))); }
    void lineWidth(double w) override { nvgStrokeWidth(vg_, f(std::max(0.0, w))); }
    void lineCap(S::LineCap cap) override {
        nvgLineCap(vg_, cap == S::LineCap::Round    ? NVG_ROUND
                        : cap == S::LineCap::Square ? NVG_SQUARE
                                                    : NVG_BUTT);
    }
    void lineJoin(S::LineJoin join) override {
        nvgLineJoin(vg_, join == S::LineJoin::Round   ? NVG_ROUND
                         : join == S::LineJoin::Bevel ? NVG_BEVEL
                                                      : NVG_MITER);
    }
    void linearGradient(double x0, double y0, double x1, double y1, int from, int to) override {
        nvgFillPaint(vg_,
                     nvgLinearGradient(vg_, f(x0), f(y0), f(x1), f(y1), colour(from), colour(to)));
    }
    void radialGradient(double x, double y, double inner, double outer, int from, int to) override {
        nvgFillPaint(
            vg_, nvgRadialGradient(vg_, f(x), f(y), f(inner), f(outer), colour(from), colour(to)));
    }
    void fillRect(double x, double y, double w, double h) override {
        nvgBeginPath(vg_);
        nvgRect(vg_, f(x), f(y), f(w), f(h));
        nvgFill(vg_);
    }
    void strokeRect(double x, double y, double w, double h) override {
        nvgBeginPath(vg_);
        nvgRect(vg_, f(x), f(y), f(w), f(h));
        nvgStroke(vg_);
    }
    void clearRect(double x, double y, double w, double h) override {
        nvgSave(vg_);
        nvgGlobalAlpha(vg_, 1.0f);
        nvgBeginPath(vg_);
        nvgRect(vg_, f(x), f(y), f(w), f(h));
        nvgFillColor(vg_, gui::rgb(theme_.ground));
        nvgFill(vg_);
        nvgRestore(vg_);
    }
    void beginPath() override { nvgBeginPath(vg_); }
    void closePath() override { nvgClosePath(vg_); }
    void moveTo(double x, double y) override { nvgMoveTo(vg_, f(x), f(y)); }
    void lineTo(double x, double y) override { nvgLineTo(vg_, f(x), f(y)); }
    void bezierCurveTo(double c1x, double c1y, double c2x, double c2y, double x,
                       double y) override {
        nvgBezierTo(vg_, f(c1x), f(c1y), f(c2x), f(c2y), f(x), f(y));
    }
    void quadraticCurveTo(double cx, double cy, double x, double y) override {
        nvgQuadTo(vg_, f(cx), f(cy), f(x), f(y));
    }
    void arc(double x, double y, double radius, double start, double end) override {
        nvgArc(vg_, f(x), f(y), f(std::max(0.0, radius)), f(start), f(end), NVG_CW);
    }
    void arcTo(double x1, double y1, double x2, double y2, double radius) override {
        nvgArcTo(vg_, f(x1), f(y1), f(x2), f(y2), f(std::max(0.0, radius)));
    }
    void rect(double x, double y, double w, double h) override {
        nvgRect(vg_, f(x), f(y), f(w), f(h));
    }
    void roundRect(double x, double y, double w, double h, double radius) override {
        nvgRoundedRect(vg_, f(x), f(y), f(w), f(h), f(std::max(0.0, radius)));
    }
    void ellipse(double x, double y, double rx, double ry) override {
        nvgEllipse(vg_, f(x), f(y), f(rx), f(ry));
    }
    void circle(double x, double y, double radius) override {
        nvgCircle(vg_, f(x), f(y), f(std::max(0.0, radius)));
    }
    void fill() override { nvgFill(vg_); }
    void stroke() override { nvgStroke(vg_); }
    void clip(double x, double y, double w, double h) override {
        nvgIntersectScissor(vg_, f(x), f(y), f(std::max(0.0, w)), f(std::max(0.0, h)));
    }
    void font(Face face, double size) override {
        nvgFontFace(vg_, face == Face::Bold   ? gui::font::kUiBold
                         : face == Face::Mono ? gui::font::kMono
                                              : gui::font::kUi);
        nvgFontSize(vg_, f(std::clamp(size, 1.0, 400.0)));
    }
    void textAlign(S::TextAlign align) override {
        align_ = align == S::TextAlign::Center  ? NVG_ALIGN_CENTER
                 : align == S::TextAlign::Right ? NVG_ALIGN_RIGHT
                                                : NVG_ALIGN_LEFT;
        nvgTextAlign(vg_, align_ | baseline_);
    }
    void textBaseline(S::TextBaseline baseline) override {
        baseline_ = baseline == S::TextBaseline::Top      ? NVG_ALIGN_TOP
                    : baseline == S::TextBaseline::Middle ? NVG_ALIGN_MIDDLE
                    : baseline == S::TextBaseline::Bottom ? NVG_ALIGN_BOTTOM
                                                          : NVG_ALIGN_BASELINE;
        nvgTextAlign(vg_, align_ | baseline_);
    }
    void fillText(const char* text, double x, double y) override {
        nvgText(vg_, f(x), f(y), text, nullptr);
    }
    double measureText(const char* text) override {
        return nvgTextBounds(vg_, 0.0f, 0.0f, text, nullptr, nullptr);
    }
    void panel(double x, double y, double w, double h, const char* title) override {
        gui::widgets::panel(vg_, theme_, box(x, y, w, h), title && *title ? title : nullptr);
    }
    void well(double x, double y, double w, double h) override {
        gui::widgets::well(vg_, theme_, box(x, y, w, h));
    }
    void meter(double db, double x, double y, double w, double h) override {
        const gui::Rect r = box(x, y, w, h);
        gui::widgets::well(vg_, theme_, r, 2.0f);
        const float level =
            gui::widgets::meterPosition(static_cast<float>(std::isfinite(db) ? db : -200.0));
        const float zero = gui::widgets::meterPosition(0.0f);
        const float top = r.bottom() - r.h * level;
        nvgBeginPath(vg_);
        nvgRect(vg_, r.x + 1.0f, top, r.w - 2.0f, r.bottom() - top);
        nvgFillColor(vg_, gui::rgb(theme_.accent));
        nvgFill(vg_);
        if (level > zero) {  // over full scale
            nvgBeginPath(vg_);
            nvgRect(vg_, r.x + 1.0f, top, r.w - 2.0f, r.h * (level - zero));
            nvgFillColor(vg_, gui::rgb(theme_.alert));
            nvgFill(vg_);
        }
    }
    void frequencyGrid(double x, double y, double w, double h, double lowHz,
                       double highHz) override {
        if (!(lowHz > 0.0 && highHz > lowHz)) return;
        const gui::Rect r = box(x, y, w, h);
        gui::widgets::frequencyGrid(vg_, theme_, r,
                                    gui::Axis::logarithmic(lowHz, highHz, r.x, r.right()));
    }
    void levelGrid(double x, double y, double w, double h, double lowDb, double highDb,
                   double stepDb) override {
        if (!(highDb > lowDb) || !(stepDb > 0.0) || (highDb - lowDb) / stepDb > 200.0) return;
        const gui::Rect r = box(x, y, w, h);
        gui::widgets::levelGrid(vg_, theme_, r, gui::Axis::linear(lowDb, highDb, r.bottom(), r.y),
                                stepDb);
    }

  private:
    static float f(double v) {
        return std::isfinite(v) ? static_cast<float>(std::clamp(v, -1e6, 1e6)) : 0.0f;
    }
    static gui::Rect box(double x, double y, double w, double h) {
        return {f(x), f(y), f(std::max(0.0, w)), f(std::max(0.0, h))};
    }

    NVGcontext* vg_;
    const gui::Theme& theme_;
    double width_, height_;
    int depth_ = 0;
    int align_ = NVG_ALIGN_LEFT, baseline_ = NVG_ALIGN_BASELINE;
};

class ScriptEditor final : public gui::EditorBase {
  public:
    ScriptEditor(gui::EditorHost& host, std::unique_ptr<Copy> copy, Recorder layout,
                 std::shared_ptr<SharedChannel> channel, bool stopped)
        : EditorBase(host, product::kTheme, static_cast<float>(layout.width),
                     static_cast<float>(layout.height)),
          copy_(std::move(copy)),
          channel_(std::move(channel)),
          stopped_(stopped) {
        const ParamTable& table = paramTable();
        auto id = [&](int slot) {
            return slot >= 0 && static_cast<size_t>(slot) < table.count()
                       ? table.at(static_cast<size_t>(slot)).id
                       : gui::kNoParam;
        };
        bool meters = false;
        for (const Recorder::Item& i : layout.items) {
            using Kind = Recorder::Item::Kind;
            switch (i.kind) {
                case Kind::Control: {
                    const uint32_t p = id(i.slots[0]);
                    if (p == gui::kNoParam) break;
                    const ParamDef& def = *table.find(p);
                    switch (i.control) {
                        case D::EditorBuilder::Control::Knob:
                            ui().knob(p, i.at);
                            break;
                        case D::EditorBuilder::Control::Slider:
                            if (i.at.w >= i.at.h)
                                ui().hslider(p, i.at);
                            else
                                ui().vslider(p, i.at);
                            break;
                        case D::EditorBuilder::Control::Toggle:
                            ui().toggle(p, i.at);
                            break;
                        case D::EditorBuilder::Control::Choice:
                            if (def.hostMax() - def.hostMin() < 4.0)
                                ui().segmented(p, i.at);
                            else
                                ui().dropdown(p, i.at);
                            break;
                        case D::EditorBuilder::Control::Readout:
                            ui().readout(p, i.at);
                            break;
                    }
                    break;
                }
                case Kind::XyPad:
                    if (id(i.slots[0]) != gui::kNoParam && id(i.slots[1]) != gui::kNoParam)
                        ui().xyPad(id(i.slots[0]), id(i.slots[1]), i.at);
                    break;
                case Kind::Envelope:
                    ui().envelope(i.at, id(i.slots[0]), id(i.slots[1]), id(i.slots[2]),
                                  id(i.slots[3]));
                    break;
                case Kind::File: {
                    const auto offset = static_cast<const unsigned char*>(i.member) -
                                        static_cast<const unsigned char*>(copy_->self());
                    for (const D::FileModel& m : copy_->code().files())
                        if (m.offset == offset && definition().properties().indexOf(m.id) >= 0)
                            ui().fileSlot(m.id, i.at);
                    break;
                }
                case Kind::Keyboard:
                    if (definition().info().takesNotes()) ui().keyboard(i.at, i.low, i.high);
                    break;
                case Kind::Meters:
                    meters_.push_back(i.at);
                    meters = true;
                    break;
            }
        }
        usePeakMeters(meters);
        // Drawing that reads a meter() repaints as it moves.
        if (copy_->code().draw())
            for (size_t k = 0; k < table.count(); ++k)
                if (table.at(k).isOutput()) watch(table.at(k).id);
        // pointer() edits through Ui, which keeps the gesture rules: a
        // hidden control for each parameter, after the script's own.
        if (copy_->code().pointer()) {
            hidden_.assign(table.count(), -1);
            for (size_t k = 0; k < table.count(); ++k) {
                if (table.at(k).isOutput()) continue;
                hidden_[k] = ui().count();
                ui().knob(table.at(k).id, {0.0f, 0.0f, 1.0f, 1.0f}).visible = false;
            }
        }
        report("editor()");
    }

    // --- pointer() --------------------------------------------------------------
    // Presses and the wheel where no control is, and every move, go to the
    // script; a press it gets holds the pointer until its release.

    bool onPointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) override {
        if (!copy_->code().pointer() || stopped_ || button != 0 || overControl(x, y)) return false;
        pressed_ = true;
        point(S::PointerAction::Press, x, y, mods, 0.0, doubleClick);
        return true;
    }
    bool onPointerMove(float x, float y, uint32_t mods) override {
        if (!copy_->code().pointer() || stopped_) return false;
        point(pressed_ ? S::PointerAction::Drag : S::PointerAction::Move, x, y, mods, 0.0, false);
        return pressed_;  // a hover is the controls' too
    }
    bool onPointerUp(float x, float y, int button, uint32_t mods) override {
        if (button != 0 || !pressed_) return true;
        pressed_ = false;
        point(S::PointerAction::Release, x, y, mods, 0.0, false);
        ui().endDragEdit();
        if (left_) {
            left_ = false;
            point(S::PointerAction::Leave, lastX_, lastY_, 0, 0.0, false);
        }
        return true;
    }
    bool onWheel(float x, float y, float dx, float dy, uint32_t mods) override {
        (void)dx;
        if (!copy_->code().pointer() || stopped_ || overControl(x, y)) return false;
        point(S::PointerAction::Wheel, x, y, mods, dy, false);
        return true;
    }
    void onPointerLeave() override {
        if (pressed_)
            left_ = true;  // told after the release
        else if (copy_->code().pointer() && !stopped_)
            point(S::PointerAction::Leave, lastX_, lastY_, 0, 0.0, false);
    }
    // Focus lost mid-press: the script hears a release (its edits ignored),
    // and Ui ends the gesture.
    void onCancel() override {
        if (!pressed_) return;
        pressed_ = left_ = false;
        point(S::PointerAction::Release, lastX_, lastY_, 0, 0.0, false, false);
    }

    void animate(double seconds) override {
        (void)seconds;
        if (channel_ && channel_->take(arrived_) && arrived_ != shown_) {
            shown_ = arrived_;
            channel_->scatter(shown_, copy_->self());
            markDirty();
        }
    }

    void paint(NVGcontext* vg) override {
        const gui::Theme& t = theme();
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, width(), height());
        nvgFillColor(vg, gui::rgb(t.ground));
        nvgFill(vg);
        if (auto hook = copy_->code().draw(); hook && !stopped_) {
            copy_->listen(host());
            Painter painter(vg, t, width(), height());
            S::Canvas canvas;
            D::Layout::bind(canvas, &painter);
            stopped_ = !copy_->run(hook, &canvas);
        }
        report("draw()");
        if (stopped_) {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "The editor's code ran past its loop budget (line %u) and was stopped.",
                          copy_->status().budgetLine.load(std::memory_order_relaxed));
            gui::widgets::label(vg, 16.0f, height() - 16.0f, line, 12.0f, gui::rgb(t.alert));
        }
        for (const gui::Rect& r : meters_) {
            const float half = std::max(1.0f, (r.w - 6.0f) * 0.5f);
            gui::widgets::meter(vg, t, {r.x, r.y, half, r.h}, inputMeters(), meterChannels());
            gui::widgets::meter(vg, t, {r.right() - half, r.y, half, r.h}, outputMeters(),
                                meterChannels());
        }
        drawControls(vg);
    }

  private:
    bool overControl(float x, float y) const {
        for (int i = 0; i < ui().count(); ++i)
            if (ui().interactive(i) && ui().control(i).bounds.contains(x, y)) return true;
        return false;
    }

    // pointer() told what the pointer did, then its edits sent to the host
    // through Ui: a press's as one gesture until the release, a wheel's as
    // one per burst.
    void point(S::PointerAction action, float x, float y, uint32_t mods, double wheel,
               bool doubleClick, bool editing = true) {
        using A = S::PointerAction;
        if (stopped_) return;
        const bool moved = action == A::Drag || action == A::Move;
        S::Pointer p;
        D::Layout::set(p, action, x, y, moved ? x - lastX_ : 0.0, moved ? y - lastY_ : 0.0, wheel,
                       doubleClick, (mods & gui::mod::kShift) != 0,
                       (mods & (gui::mod::kCtrl | gui::mod::kSuper)) != 0,
                       (mods & gui::mod::kAlt) != 0);
        lastX_ = x;
        lastY_ = y;
        copy_->listen(host());
        copy_->allowEdits(editing && action != A::Move && action != A::Leave);
        stopped_ = !copy_->run(copy_->code().pointer(), &p);
        copy_->allowEdits(false);
        report("pointer()");
        for (const Copy::Edited& e : copy_->takeEdits()) {
            if (action == A::Wheel)
                ui().wheelEdit(hidden_[e.index], e.host);
            else
                ui().dragEdit(hidden_[e.index], e.host);
        }
        markDirty();
    }

    // What went wrong in the editor's code, in the host's log, once each.
    void report(const char* hook) {
        const D::Status& s = copy_->status();
        const std::uint32_t budget = s.budgetExceeded.load(std::memory_order_relaxed);
        const std::uint32_t range = s.outOfRange.load(std::memory_order_relaxed);
        char line[200];
        if (budget != budgetReported_) {
            budgetReported_ = budget;
            std::snprintf(line, sizeof(line),
                          "%s ran past its loop budget at line %u and was stopped", hook,
                          s.budgetLine.load(std::memory_order_relaxed));
            host().log(gui::LogLevel::Warning, line);
        }
        if (range != rangeReported_ && rangeReported_ == 0) {
            std::snprintf(line, sizeof(line), "%s indexed outside an array at line %u", hook,
                          s.outOfRangeLine.load(std::memory_order_relaxed));
            host().log(gui::LogLevel::Warning, line);
        }
        rangeReported_ = range;
    }

    std::unique_ptr<Copy> copy_;
    std::shared_ptr<SharedChannel> channel_;
    std::vector<unsigned char> arrived_, shown_;
    std::vector<gui::Rect> meters_;
    std::vector<int> hidden_;  // each parameter's hidden control, by table index
    bool pressed_ = false;     // a press pointer() got, until its release
    bool left_ = false;        // the pointer left during that press
    float lastX_ = 0.0f, lastY_ = 0.0f;
    bool stopped_ = false;
    std::uint32_t budgetReported_ = 0, rangeReported_ = 0;
};

}  // namespace

std::unique_ptr<gui::EditorBase> makeScriptEditor(gui::EditorHost& host,
                                                  std::shared_ptr<const D::Code> code,
                                                  std::shared_ptr<SharedChannel> channel) {
    if (!code || (!code->editor() && !code->draw())) return nullptr;
    auto copy = std::make_unique<Copy>(code, host.pluginDefinition().params(), host.sampleRate());
    Recorder layout;
    bool stopped = false;
    if (auto hook = code->editor()) {
        copy->listen(host);
        S::Editor e;
        D::Layout::bind(e, &layout);
        stopped = !copy->run(hook, &e);
    }
    return std::make_unique<ScriptEditor>(host, std::move(copy), std::move(layout),
                                          std::move(channel), stopped);
}

}  // namespace supple::suite
