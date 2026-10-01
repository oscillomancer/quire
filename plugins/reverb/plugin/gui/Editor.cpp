// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char reverb_font_mono[];
extern const unsigned int reverb_font_mono_size;
extern const unsigned char reverb_font_mono_medium[];
extern const unsigned int reverb_font_mono_medium_size;
extern const unsigned char reverb_font_display[];
extern const unsigned int reverb_font_display_size;
extern const unsigned char reverb_font_ui[];
extern const unsigned int reverb_font_ui_size;
extern const unsigned char reverb_font_ui_semibold[];
extern const unsigned int reverb_font_ui_semibold_size;
}

namespace supple::reverb::gui {
namespace {
constexpr uint32_t kEditable[] = {
    kSize,   kDecay,    kPredelay,  kDiffusion, kEarly,       kModulation, kWidth,   kMix,
    kOutput, kLowDecay, kCrossover, kHighDecay, kDampingFreq, kLowCut,     kHighCut, kFreeze};
struct ControlGeom {
    uint32_t id;
    float x, y, r;
};
constexpr ControlGeom kControls[] = {
    {kSize, 665, 153, 34},        {kDecay, 805, 153, 34},     {kPredelay, 75, 348, 24},
    {kDiffusion, 200, 348, 24},   {kEarly, 325, 348, 24},     {kModulation, 450, 348, 24},
    {kWidth, 575, 348, 24},       {kMix, 700, 348, 24},       {kOutput, 825, 348, 24},
    {kLowDecay, 75, 477, 20},     {kCrossover, 200, 477, 20}, {kHighDecay, 325, 477, 20},
    {kDampingFreq, 450, 477, 20}, {kLowCut, 575, 477, 20},    {kHighCut, 700, 477, 20},
    {kFreeze, 825, 477, 20}};
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(reverb_font_mono),
                                   reverb_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(reverb_font_mono_medium),
                         reverb_font_mono_medium_size, 0);
    const int c =
        nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(reverb_font_display),
                         reverb_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(reverb_font_ui),
                                   reverb_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(reverb_font_ui_semibold),
                         reverb_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    if (fontsReady_) {
        // The bundled Plex subsets omit infinity; Inter contains the glyph.
        nvgAddFallbackFontId(vg, a, e);
        nvgAddFallbackFontId(vg, b, e);
    }
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0., .25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        latestIn_ = f.inputPeakDb;
        latestOut_ = f.outputPeakDb;
        dirty_ = true;
    }
    if (scrollId_ >= 0) {
        scrollIdle_ += dt;
        if (scrollIdle_ >= .25f) closeScrollGesture();
    }
    for (const auto& p : kParams)
        if (shown_[p.id] != host_.paramValue(p.id)) dirty_ = true;
}
bool Editor::wantsRepaint() const {
    return dirty_ || host_.telemetry().pending() || scrollId_ >= 0;
}
void Editor::render(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kDesignH);
    nvgFillColor(vg, rgb(col::ink));
    nvgFill(vg);
    label(vg, "Supple Audio", 20, 19, 10, rgb(col::dust));
    label(vg, kName, 20, 43, 25, rgb(col::bone), Align::Left, true);
    label(vg, "REVERB", 880, 36, 11, rgb(col::accent), Align::Right, true);
    hairline(vg, 0, 62.5f, 900, 62.5f, rgb(col::edgeHi));
    cell(vg, 20, 80, 545, 197, "Decay target · seconds / Hz");
    Values values{};
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = host_.paramValue(static_cast<uint32_t>(i));
    const double maximum =
        std::max(1., std::pow(10., std::ceil(std::log10(values[kDecay] * .001 *
                                                        std::max(1., values[kLowDecay])))));
    const double minimum = maximum * .001;
    auto fy = [&](double t) {
        return 247.f - static_cast<float>(std::clamp(std::log10(t / minimum) / 3, 0., 1.)) * 129;
    };
    auto fx = [](double f) {
        return 66.f + static_cast<float>(std::log(f / 20) / std::log(1000.)) * 477;
    };
    for (int i = 0; i < 4; ++i) {
        const double t = minimum * std::pow(10., i);
        float y = fy(t);
        hairline(vg, 66, y, 543, y, rgba(col::edgeHi, .6f));
        char text[16];
        std::snprintf(text, sizeof(text), "%.3g", t);
        num(vg, text, 57, y, 9, rgb(col::dust), Align::Right);
    }
    for (double f : {20., 100., 1000., 10000., 20000.}) {
        float x = fx(f);
        hairline(vg, x, 118, x, 247, rgba(col::edgeHi, .5f));
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f", f);
        num(vg, text, x, 264, 9, rgb(col::dust), Align::Center);
    }
    for (auto id : {kCrossover, kDampingFreq}) {
        float x = fx(values[id]);
        hairline(vg, x, 118, x, 247, rgba(col::cool, .45f));
    }
    nvgBeginPath(vg);
    for (int i = 0; i <= 240; ++i) {
        const double f = 20 * std::pow(1000., static_cast<double>(i) / 240);
        float x = fx(f), y = fy(decaySeconds(values, f, host_.sampleRate()));
        if (i == 0)
            nvgMoveTo(vg, x, y);
        else
            nvgLineTo(vg, x, y);
    }
    nvgStrokeWidth(vg, 2);
    nvgStrokeColor(vg, rgb(col::accent));
    nvgStroke(vg);
    // Input-bandwidth trace shares the plot but has its own -48..0 dB scale.
    nvgBeginPath(vg);
    for (int i = 0; i <= 240; ++i) {
        double f = 20 * std::pow(1000., static_cast<double>(i) / 240);
        double db = -10 * std::log10((1 + std::pow(values[kLowCut] / f, 4)) *
                                     (1 + std::pow(f / values[kHighCut], 4)));
        float x = fx(f), y = 118 + static_cast<float>(std::clamp(-db, 0., 48.)) * 129 / 48;
        if (i == 0)
            nvgMoveTo(vg, x, y);
        else
            nvgLineTo(vg, x, y);
    }
    nvgStrokeWidth(vg, 1);
    nvgStrokeColor(vg, rgba(col::cool, .5f));
    nvgStroke(vg);
    cell(vg, 585, 80, 295, 197, "Space");
    label(vg, "Input filters · 0 to −48 dB", 543, 96, 9, rgb(col::cool), Align::Right);
    char peaks[80];
    std::snprintf(peaks, sizeof(peaks), "In %+.1f / Out %+.1f dBFS", latestIn_, latestOut_);
    label(vg, peaks, 733, 254, 10, rgb(col::dust), Align::Center);
    cell(vg, 20, 296, 860, 117, "Onset / texture / output");
    cell(vg, 20, 426, 485, 105, "Frequency-dependent decay");
    cell(vg, 520, 426, 360, 105, "Input bandwidth / hold");
    for (const auto& g : kControls) {
        const auto* p = findParam(g.id);
        double value = host_.paramValue(g.id);
        if (p->type == params::Type::Float) {
            knob(vg, g.x, g.y, g.r, static_cast<float>(toNormalised(*p, value)), p->name.data(), "",
                 "", p->min < 0 && p->max > 0 && g.id != kEarly, hover_ == static_cast<int>(g.id));
            const auto text = formatValue(g.id, value);
            num(vg, text.c_str(), g.x, g.y + g.r + 31, 10.5f, rgb(col::bone), Align::Center);
        } else {
            label(vg, p->name.data(), g.x, g.y - 26, 10, rgb(col::dust), Align::Center);
            pill(vg, g.x - 30, g.y - 10, 60, 22, value > .5 ? "Held" : "Off", value > .5,
                 rgb(col::accent), true, hover_ == static_cast<int>(g.id));
        }
    }
    label(vg, "16-line diffusion network", 20, 546, 9, rgb(col::dim));
    label(vg, values[kFreeze] > .5 ? "Infinite tail · zero latency" : "Zero latency", 880, 546, 9,
          rgb(col::dim), Align::Right);
    drawFocus(vg);
    for (const auto& p : kParams) shown_[p.id] = host_.paramValue(p.id);
    dirty_ = false;
}

int Editor::controlAt(float x, float y) const {
    for (const auto& g : kControls) {
        if (!controlEnabled(g.id)) continue;
        const float dx = x - g.x, dy = y - g.y;
        if (dx * dx + dy * dy <= (g.r + 14) * (g.r + 14)) return static_cast<int>(g.id);
    }
    return -1;
}

bool Editor::controlEnabled(uint32_t id) const { return id < kNumParams; }

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
    if (p->type != params::Type::Float) {
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
    if (!controlEnabled(static_cast<uint32_t>(dragId_))) {
        onMouseUp();
        return;
    }
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
    if (!p || (p->flags & params::kReadonly)) return;
    if (dragId_ >= 0) onMouseUp();
    host_.beginEdit(p->id);
    host_.setParam(p->id, p->def);
    host_.endEdit(p->id);
    focusParam(p->id);
}

bool Editor::onScroll(float x, float y, float dy) {
    const int id = controlAt(x, y);
    const ParamDef* p = id >= 0 ? findParam(static_cast<uint32_t>(id)) : nullptr;
    if (!p || (p->flags & params::kReadonly) || p->type != params::Type::Float) return false;
    if (scrollId_ != id) {
        closeScrollGesture();
        scrollId_ = id;
        host_.beginEdit(p->id);
    }
    scrollIdle_ = 0.0f;
    focusParam(p->id);
    setFromNormal(p->id, toNormalised(*p, host_.paramValue(p->id)) - dy * 0.025);
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
        int index = -1;
        for (int i = 0; i < kNumParams; ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        for (int visited = 0; visited < kNumParams; ++visited) {
            index = index < 0 ? (fineOrReverse ? kNumParams - 1 : 0)
                              : (index + (fineOrReverse ? kNumParams - 1 : 1)) % kNumParams;
            if (controlEnabled(kEditable[index])) {
                focusParam(kEditable[index]);
                break;
            }
        }
        return;
    }
    if (focus_ < 0) return;
    const ParamDef* p = findParam(static_cast<uint32_t>(focus_));
    if (!p || (p->flags & params::kReadonly) || !controlEnabled(p->id)) return;
    if ((key == EditorKey::Enter || key == EditorKey::Space) && p->type != params::Type::Float) {
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
        normal -= fineOrReverse ? 0.002 : 0.01;
    else if (key == EditorKey::Right || key == EditorKey::Up)
        normal += fineOrReverse ? 0.002 : 0.01;
    else
        return;
    host_.beginEdit(p->id);
    if (p->type != params::Type::Float && key != EditorKey::Home && key != EditorKey::End)
        host_.setParam(p->id,
                       std::clamp(host_.paramValue(p->id) +
                                      (key == EditorKey::Left || key == EditorKey::Down ? -1. : 1.),
                                  p->min, p->max));
    else
        setFromNormal(p->id, normal);
    host_.endEdit(p->id);
}

void Editor::drawFocus(NVGcontext* vg) {
    if (focus_ < 0 || !controlEnabled(static_cast<uint32_t>(focus_))) return;
    for (const auto& g : kControls)
        if (static_cast<int>(g.id) == focus_) {
            nvgBeginPath(vg);
            nvgCircle(vg, g.x, g.y, g.r + 8);
            nvgStrokeColor(vg, rgba(col::bone, 0.7f));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
}

void Editor::onFocusLost() {
    onMouseUp();
    closeScrollGesture();
    focus_ = -1;
    hover_ = -1;
    dirty_ = true;
}

}  // namespace supple::reverb::gui
