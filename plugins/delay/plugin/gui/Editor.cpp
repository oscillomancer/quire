// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char delay_font_mono[];
extern const unsigned int delay_font_mono_size;
extern const unsigned char delay_font_mono_medium[];
extern const unsigned int delay_font_mono_medium_size;
extern const unsigned char delay_font_display[];
extern const unsigned int delay_font_display_size;
extern const unsigned char delay_font_ui[];
extern const unsigned int delay_font_ui_size;
extern const unsigned char delay_font_ui_semibold[];
extern const unsigned int delay_font_ui_semibold_size;
}

namespace supple::delay::gui {
namespace {
namespace pa = supple::params;
constexpr uint32_t kEditable[] = {kSync,     kTime,    kMode,   kOffset,  kFeedback,
                                  kHighpass, kLowpass, kFreeze, kModRate, kModDepth,
                                  kDrive,    kDuck,    kWidth,  kMix,     kOutput};
struct ControlGeom {
    uint32_t id;
    float x, y, w;
};
constexpr ControlGeom kControls[] = {
    {kSync, 30, 328, 88},      {kTime, 134, 328, 88},     {kMode, 238, 328, 88},
    {kOffset, 342, 328, 88},   {kFeedback, 446, 328, 88}, {kHighpass, 550, 328, 88},
    {kLowpass, 654, 328, 88},  {kFreeze, 758, 328, 88},   {kModRate, 30, 438, 88},
    {kModDepth, 134, 438, 88}, {kDrive, 238, 438, 88},    {kDuck, 342, 438, 88},
    {kWidth, 446, 438, 88},    {kMix, 550, 438, 88},      {kOutput, 654, 438, 88}};
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(delay_font_mono),
                                   delay_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(delay_font_mono_medium),
                         delay_font_mono_medium_size, 0);
    const int c = nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(delay_font_display),
                                   delay_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(delay_font_ui),
                                   delay_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(delay_font_ui_semibold),
                         delay_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        latest_ = f;
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
    fillRoundRect(vg, 0, 0, kDesignW, kDesignH, 0, rgb(col::ink));
    label(vg, "Supple Audio", 30, 22, 10, rgb(col::dust));
    label(vg, kName, 30, 49, 25, rgb(col::bone));
    label(vg, "Stereo delay / circulating echoes", 870, 40, 11, rgb(col::accent), Align::Right);
    Values values{};
    for (const auto& p : kParams) values[p.id] = host_.paramValue(p.id);
    auto times = lineTimes(values, latest_.tempo);
    if (values[kFreeze]) times = {latest_.leftMs, latest_.rightMs};
    cell(vg, 30, 82, 580, 220, "Echo pattern / first repeats");
    const double horizon = std::max(times[0], times[1]) * 6;
    const auto x = [&](double ms) { return 72 + float(ms / horizon) * 512; };
    for (int i = 0; i <= 4; ++i) {
        const float gx = 72 + 128.f * i;
        hairline(vg, gx, 130, gx, 269, rgba(col::edgeHi, .5f));
        char t[24];
        std::snprintf(t, sizeof(t), "%.2f s", horizon * i / 4000);
        num(vg, t, gx, 286, 9, rgb(col::dim), Align::Center);
    }
    label(vg, "L", 48, 164, 12, rgb(col::accent));
    label(vg, "R", 48, 240, 12, rgb(col::cool));
    hairline(vg, 72, 186, 584, 186, rgb(col::edgeHi));
    hairline(vg, 72, 262, 584, 262, rgb(col::edgeHi));
    const bool ping = values[kMode] == 1, mono = values[kMode] == 2;
    for (int repeat = 1; repeat <= 6; ++repeat) {
        const double level = values[kFreeze] ? 1 : std::pow(values[kFeedback] / 100, repeat - 1);
        for (int c = 0; c < 2; ++c) {
            if (ping && ((repeat - 1) % 2) != c) continue;
            const double ms =
                ping ? std::ceil(repeat / 2.) * times[0] + std::floor(repeat / 2.) * times[1]
                     : repeat * times[mono ? 0 : c];
            if (ms > horizon) continue;
            const float base = c == 0 ? 186 : 262, height = 45 * float(std::min(level, 1.));
            hairline(vg, x(ms), base, x(ms), base - height, rgb(c == 0 ? col::accent : col::cool),
                     3);
            nvgBeginPath(vg);
            nvgCircle(vg, x(ms), base - height, 3);
            nvgFillColor(vg, rgb(c == 0 ? col::accent : col::cool));
            nvgFill(vg);
        }
    }
    cell(vg, 630, 82, 240, 220, "Loop filters / 12 dB per octave");
    for (int d : {0, -12, -24}) {
        const float y = 134 - float(d) * 4;
        hairline(vg, 650, y, 850, y, rgba(col::edgeHi, .5f));
        char t[12];
        std::snprintf(t, sizeof(t), "%d", d);
        num(vg, t, 851, y - 8, 9, rgb(col::dim), Align::Right);
    }
    nvgBeginPath(vg);
    for (int i = 0; i < 201; ++i) {
        const double hz = 20 * std::pow(1000., i / 200.);
        const double gain =
            loopMagnitude(hz, values[kHighpass], values[kLowpass], host_.sampleRate());
        const float y =
            134 - float(std::clamp(20 * std::log10(std::max(gain, 1e-6)), -36., 0.)) * 3.5f;
        const float fx = 650 + float(i);
        if (i == 0)
            nvgMoveTo(vg, fx, y);
        else
            nvgLineTo(vg, fx, y);
    }
    nvgStrokeColor(vg, rgb(col::accent));
    nvgStrokeWidth(vg, 1.8f);
    nvgStroke(vg);
    label(vg, "20 Hz", 650, 281, 9, rgb(col::dim));
    label(vg, "20 kHz", 850, 281, 9, rgb(col::dim), Align::Right);
    for (const auto& g : kControls) {
        const auto& p = table()[g.id];
        const double value = values[g.id];
        const auto text = pa::format(p, value);
        if (p.type == pa::Type::Float) {
            const float radius = g.id == kTime || g.id == kFeedback ? 28 : 22;
            knob(vg, g.x + 44, g.y + 26, radius, float(toNormalised(p, value)), p.name.data(), "",
                 "", false, hover_ == int(g.id), p.min < 0 && p.max > 0);
            num(vg, text.c_str(), g.x + 44, g.y + 83, 11.5f, rgb(col::bone), Align::Center);
        } else {
            label(vg, p.name.data(), g.x + 44, g.y + 63, 10, rgb(col::dust), Align::Center);
            pill(vg, g.x, g.y + 12, g.w, 28, text.c_str(), value != 0, rgb(col::accent),
                 p.type == pa::Type::Bool, hover_ == int(g.id));
        }
    }
    char tempo[80];
    std::snprintf(tempo, sizeof(tempo), "%.1f BPM / latency 0 samples", latest_.tempo);
    label(vg, tempo, 30, kFooterTop, 10, rgb(col::dim));
    char meter[80];
    std::snprintf(meter, sizeof(meter), "Wet %.1f dBFS / duck %.1f dB", latest_.outputPeakDb,
                  latest_.duckDb);
    label(vg, meter, 870, kFooterTop, 10, rgb(col::dust), Align::Right);
    drawFocus(vg);
    for (const auto& p : kParams) shown_[p.id] = values[p.id];
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
        for (int i = 0; i < 15; ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        index = focus_ < 0 ? (fineOrReverse ? 14 : 0) : (index + (fineOrReverse ? 14 : 1)) % 15;
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

}  // namespace supple::delay::gui
