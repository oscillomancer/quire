// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char gate_font_mono[];
extern const unsigned int gate_font_mono_size;
extern const unsigned char gate_font_mono_medium[];
extern const unsigned int gate_font_mono_medium_size;
extern const unsigned char gate_font_display[];
extern const unsigned int gate_font_display_size;
extern const unsigned char gate_font_ui[];
extern const unsigned int gate_font_ui_size;
extern const unsigned char gate_font_ui_semibold[];
extern const unsigned int gate_font_ui_semibold_size;
}

namespace gate::gui {
namespace {
constexpr uint32_t kEditable[] = {
    kMode,      kThreshold, kRange,      kRatio,     kHysteresis, kAttack,     kHold, kRelease,
    kLookahead, kSidechain, kScHighpass, kScLowpass, kScListen,   kStereoLink, kMix,  kOutput};
struct ControlGeom {
    uint32_t id;
    float x, y, w;
};
constexpr ControlGeom kControls[] = {
    {kMode, 30, 328, 88},       {kThreshold, 126, 328, 88},  {kRange, 222, 328, 88},
    {kRatio, 318, 328, 88},     {kHysteresis, 414, 328, 88}, {kAttack, 510, 328, 88},
    {kHold, 606, 328, 88},      {kRelease, 702, 328, 88},    {kLookahead, 30, 438, 88},
    {kSidechain, 126, 438, 88}, {kScHighpass, 222, 438, 88}, {kScLowpass, 318, 438, 88},
    {kScListen, 414, 438, 88},  {kStereoLink, 510, 438, 88}, {kMix, 606, 438, 88},
    {kOutput, 702, 438, 88}};
float historyY(float db) { return 273 - std::clamp((db + 80) / 80, 0.f, 1.f) * 140; }
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(gate_font_mono),
                                   gate_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(gate_font_mono_medium),
                         gate_font_mono_medium_size, 0);
    const int c = nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(gate_font_display),
                                   gate_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(gate_font_ui),
                                   gate_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(gate_font_ui_semibold),
                         gate_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        if (f.seconds < latest_.seconds) {
            std::fill(std::begin(history_), std::end(history_), -80.f);
            historyElapsed_ = 0;
        }
        historyElapsed_ += std::max(0., f.seconds - latest_.seconds);
        latest_ = f;
        while (historyElapsed_ >= kHistorySeconds / kHistoryColumns) {
            history_[historyHead_] = static_cast<float>(f.gr);
            historyHead_ = (historyHead_ + 1) % kHistoryColumns;
            historyElapsed_ -= kHistorySeconds / kHistoryColumns;
        }
        dirty_ = true;
    }

    if (scrollId_ >= 0) {
        scrollIdle_ += dt;
        if (scrollIdle_ >= 0.25f) closeScrollGesture();
    }

    for (const auto& p : kParams) {
        const double now = host_.paramValue(p.id);
        if (shown_[p.id] != now) dirty_ = true;
    }
}

bool Editor::wantsRepaint() const {
    return dirty_ || host_.telemetry().pending() || scrollId_ >= 0;
}
void Editor::render(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kDesignH);
    nvgFillColor(vg, rgb(col::ink));
    nvgFill(vg);
    label(vg, "Supple Audio", 30, 22, 10, rgb(col::dust));
    label(vg, kName, 30, 49, 25, rgb(col::bone));
    label(vg, "Gate / downward expander", 790, 40, 11, rgb(col::accent), Align::Right);
    cell(vg, 30, 82, 350, 220, "Key level / hysteresis band");
    const float threshold = static_cast<float>(host_.paramValue(kThreshold)),
                close = threshold - static_cast<float>(host_.paramValue(kHysteresis));
    const auto y = [](float db) { return 277 - std::clamp((db + 80) / 80, 0.f, 1.f) * 148; };
    nvgBeginPath(vg);
    nvgRect(vg, 72, y(threshold), 276, y(close) - y(threshold));
    nvgFillColor(vg, rgba(col::accent, .18f));
    nvgFill(vg);
    for (int db : {-80, -60, -40, -20, 0}) {
        hairline(vg, 72, y(static_cast<float>(db)), 348, y(static_cast<float>(db)),
                 rgba(col::edgeHi, .6f));
        char text[12];
        std::snprintf(text, sizeof(text), "%d", db);
        num(vg, text, 61, y(static_cast<float>(db)), 10, rgb(col::dust), Align::Right);
    }
    hairline(vg, 72, y(threshold), 348, y(threshold), rgb(col::accent), 2);
    hairline(vg, 72, y(close), 348, y(close), rgba(col::accent, .6f));
    const float levelY = y(static_cast<float>(latest_.level));
    hairline(vg, 82, levelY, 338, levelY, rgb(col::cool), 3);
    char level[64];
    std::snprintf(level, sizeof(level), "Key %.1f dBFS", latest_.level);
    label(vg, level, 72, 292, 10, rgb(col::dust));
    pill(vg, 277, 91, 90, 24, latest_.open ? "OPEN" : "CLOSED", latest_.open, rgb(col::accent));
    cell(vg, 400, 82, 390, 220, "Gain reduction / 4 s");
    for (int db : {-80, -40, 0}) {
        hairline(vg, 433, historyY(static_cast<float>(db)), 772, historyY(static_cast<float>(db)),
                 rgba(col::edgeHi, .6f));
        char text[12];
        std::snprintf(text, sizeof(text), "%d", db);
        num(vg, text, 425, historyY(static_cast<float>(db)), 9, rgb(col::dim), Align::Right);
    }
    nvgBeginPath(vg);
    for (int i = 0; i < kHistoryColumns; ++i) {
        const float x = 433 + 339.f * i / (kHistoryColumns - 1),
                    hy = historyY(history_[(historyHead_ + i) % kHistoryColumns]);
        if (!i)
            nvgMoveTo(vg, x, hy);
        else
            nvgLineTo(vg, x, hy);
    }
    nvgStrokeColor(vg, rgb(col::accent));
    nvgStrokeWidth(vg, 1.8f);
    nvgStroke(vg);
    const auto gr = supple::params::format(table()[kGr], latest_.gr);
    num(vg, gr.c_str(), 772, 105, 16, rgb(col::bone), Align::Right);
    for (const auto& g : kControls) {
        const auto& p = table()[g.id];
        const double value = host_.paramValue(g.id);
        const auto text = p.type == pa::Type::Float ? supple::params::formatCompact(p, value)
                                                    : supple::params::format(p, value);
        if (p.type == pa::Type::Float) {
            knob(vg, g.x + 44, g.y + 26, 22, static_cast<float>(toNormalised(p, value)),
                 p.name.data(), text.c_str(), supple::units::unitName(p.unit).data(), false,
                 hover_ == static_cast<int>(g.id));
        } else {
            label(vg, p.name.data(), g.x + 44, g.y + 63, 10, rgb(col::dust), Align::Center);
            pill(vg, g.x, g.y + 12, g.w, 28, text.c_str(), value != 0, rgb(col::accent), true,
                 hover_ == static_cast<int>(g.id));
        }
    }
    char footer[80];
    std::snprintf(footer, sizeof(footer), "Latency %d samples / key monitor follows mix and output",
                  host_.latencySamples());
    label(vg, footer, 30, kFooterTop, 10, rgb(col::dim));
    drawFocus(vg);
    for (const auto& p : kParams) shown_[p.id] = host_.paramValue(p.id);
    dirty_ = false;
}

int Editor::controlAt(float x, float y) const {
    for (const auto& g : kControls)
        if (x >= g.x && x <= g.x + g.w && y >= g.y && y <= g.y + 95) return static_cast<int>(g.id);
    return -1;
}

void Editor::focusParam(uint32_t id) {
    focus_ = static_cast<int>(id);
    dirty_ = true;
}

bool Editor::onMouseDown(float x, float y) {
    const int id = controlAt(x, y);
    if (id < 0) return false;
    hover_ = id;
    closeScrollGesture();
    focusParam(static_cast<uint32_t>(id));
    const ParamDef* p = findParam(static_cast<uint32_t>(id));
    if (p->type != pa::Type::Float) {
        host_.beginEdit(p->id);
        host_.setParam(p->id,
                       host_.paramValue(p->id) >= p->max ? p->min : host_.paramValue(p->id) + 1);
        host_.endEdit(p->id);
    } else {
        dragId_ = id;
        dragStartY_ = y;
        dragStartNormal_ = toNormalised(*p, host_.paramValue(p->id));
        host_.beginEdit(p->id);
    }
    dirty_ = true;
    return true;
}

void Editor::onMouseMove(float x, float y, bool fine) {
    const int nextHover = controlAt(x, y);
    if (nextHover != hover_) {
        hover_ = nextHover;
        dirty_ = true;
    }
    if (dragId_ < 0) return;
    const double scale = fine ? 0.0015 : 0.006;
    setFromNormal(static_cast<uint32_t>(dragId_), dragStartNormal_ + (dragStartY_ - y) * scale);
}

void Editor::onMouseUp() {
    if (dragId_ < 0) return;
    host_.endEdit(static_cast<uint32_t>(dragId_));
    dragId_ = -1;
}

void Editor::onPointerLeave() {
    if (dragId_ >= 0) onMouseUp();
    if (hover_ >= 0) {
        hover_ = -1;
        dirty_ = true;
    }
}

void Editor::onDoubleClick(float x, float y) {
    const int id = controlAt(x, y);
    const ParamDef* p = id >= 0 ? findParam(static_cast<uint32_t>(id)) : nullptr;
    if (!p || (p->flags & pa::kReadonly)) return;
    if (dragId_ >= 0) onMouseUp();
    host_.beginEdit(p->id);
    host_.setParam(p->id, p->def);
    host_.endEdit(p->id);
    focusParam(p->id);
}

bool Editor::onScroll(float x, float y, float dy) {
    const int id = controlAt(x, y);
    const ParamDef* p = id >= 0 ? findParam(static_cast<uint32_t>(id)) : nullptr;
    if (!p || (p->flags & pa::kReadonly)) return false;
    if (scrollId_ != id) {
        closeScrollGesture();
        scrollId_ = id;
        host_.beginEdit(p->id);
    }
    scrollIdle_ = 0.0f;
    focusParam(p->id);
    if (p->type == pa::Type::Float)
        setFromNormal(p->id, toNormalised(*p, host_.paramValue(p->id)) + (dy < 0 ? .01 : -.01));
    else
        host_.setParam(p->id,
                       std::clamp(host_.paramValue(p->id) + (dy < 0 ? 1 : -1), p->min, p->max));
    return true;
}

void Editor::closeScrollGesture() {
    if (scrollId_ >= 0) host_.endEdit(static_cast<uint32_t>(scrollId_));
    scrollId_ = -1;
    scrollIdle_ = 0.0f;
}

void Editor::setFromNormal(uint32_t id, double normal) {
    if (const ParamDef* p = findParam(id)) host_.setParam(id, fromNormalised(*p, normal));
    dirty_ = true;
}

void Editor::onKey(EditorKey key, bool fineOrReverse) {
    if (key == EditorKey::Tab) {
        int index = 0;
        for (int i = 0; i < 16; ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        index = focus_ < 0 ? (fineOrReverse ? 15 : 0) : (index + (fineOrReverse ? 15 : 1)) % 16;
        focusParam(kEditable[index]);
        return;
    }
    if (focus_ < 0) return;
    const ParamDef* p = findParam(static_cast<uint32_t>(focus_));
    if (!p || (p->flags & pa::kReadonly)) return;
    if ((key == EditorKey::Enter || key == EditorKey::Space) && p->type != pa::Type::Float) {
        host_.beginEdit(p->id);
        host_.setParam(p->id,
                       host_.paramValue(p->id) >= p->max ? p->min : host_.paramValue(p->id) + 1);
        host_.endEdit(p->id);
        dirty_ = true;
        return;
    }
    double normal = toNormalised(*p, host_.paramValue(p->id));
    if (key == EditorKey::Home)
        normal = 0.0;
    else if (key == EditorKey::End)
        normal = 1.0;
    else if (key == EditorKey::Left || key == EditorKey::Down)
        normal -= p->type == pa::Type::Float ? (fineOrReverse ? .001 : .01) : 1 / (p->max - p->min);
    else if (key == EditorKey::Right || key == EditorKey::Up)
        normal += p->type == pa::Type::Float ? (fineOrReverse ? .001 : .01) : 1 / (p->max - p->min);
    else
        return;
    host_.beginEdit(p->id);
    setFromNormal(p->id, normal);
    host_.endEdit(p->id);
}

void Editor::drawFocus(NVGcontext* vg) {
    for (const auto& g : kControls)
        if (focus_ == static_cast<int>(g.id))
            strokeRoundRect(vg, g.x - 3, g.y - 3, g.w + 6, 95, 5, rgba(col::bone, .7f));
}

void Editor::onFocusLost() {
    onMouseUp();
    closeScrollGesture();
    focus_ = -1;
    hover_ = -1;
    dirty_ = true;
}

}  // namespace gate::gui
