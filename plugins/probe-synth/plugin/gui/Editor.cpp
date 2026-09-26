// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char probesynth_font_mono[];
extern const unsigned int probesynth_font_mono_size;
extern const unsigned char probesynth_font_mono_medium[];
extern const unsigned int probesynth_font_mono_medium_size;
extern const unsigned char probesynth_font_display[];
extern const unsigned int probesynth_font_display_size;
extern const unsigned char probesynth_font_ui[];
extern const unsigned int probesynth_font_ui_size;
extern const unsigned char probesynth_font_ui_semibold[];
extern const unsigned int probesynth_font_ui_semibold_size;
}

namespace probesynth::gui {
namespace {
constexpr uint32_t kEditable[] = {kWaveform,      kCutoff,       kResonance,   kFilterAmount,
                                  kFilterSlope,   kAmpAttack,    kAmpDecay,    kAmpSustain,
                                  kAmpRelease,    kFilterAttack, kFilterDecay, kFilterSustain,
                                  kFilterRelease, kOutputGain,   kSeed};
struct ControlGeom {
    uint32_t id;
    float x, y, r;
};
constexpr ControlGeom kControls[] = {
    {kWaveform, 100, 125, 28},      {kCutoff, 255, 125, 28},        {kResonance, 410, 125, 28},
    {kFilterAmount, 565, 125, 28},  {kFilterSlope, 720, 125, 28},   {kAmpAttack, 100, 245, 28},
    {kAmpDecay, 255, 245, 28},      {kAmpSustain, 410, 245, 28},    {kAmpRelease, 565, 245, 28},
    {kOutputGain, 720, 245, 28},    {kFilterAttack, 100, 365, 28},  {kFilterDecay, 255, 365, 28},
    {kFilterSustain, 410, 365, 28}, {kFilterRelease, 565, 365, 28}, {kSeed, 720, 365, 28},
};
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(probesynth_font_mono),
                                   probesynth_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(probesynth_font_mono_medium),
                         probesynth_font_mono_medium_size, 0);
    const int c =
        nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(probesynth_font_display),
                         probesynth_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(probesynth_font_ui),
                                   probesynth_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(probesynth_font_ui_semibold),
                         probesynth_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        latestIn_ = f.inputPeakDb;
        latestOut_ = f.outputPeakDb;
        if (latestIn_ >= heldIn_) {
            heldIn_ = latestIn_;
            holdAgeIn_ = 0.0f;
        }
        if (latestOut_ >= heldOut_) {
            heldOut_ = latestOut_;
            holdAgeOut_ = 0.0f;
        }
        historyFill_ += static_cast<float>(kHistoryColumns) / (250.0f * kHistorySeconds);
        while (historyFill_ >= 1.0f) {
            historyIn_[historyHead_] = latestIn_;
            historyOut_[historyHead_] = latestOut_;
            historyHead_ = (historyHead_ + 1) % kHistoryColumns;
            historyFill_ -= 1.0f;
        }
        dirty_ = true;
    }

    auto decayHold = [dt](float& value, float& age) {
        age += dt;
        if (age > 1.0f) value = std::max(-120.0f, value - 18.0f * dt);
    };
    decayHold(heldIn_, holdAgeIn_);
    decayHold(heldOut_, holdAgeOut_);

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
    if (dirty_ || host_.telemetry().pending() || scrollId_ >= 0) return true;
    return (holdAgeIn_ > 1.0f && heldIn_ > -120.0f) || (holdAgeOut_ > 1.0f && heldOut_ > -120.0f);
}

void Editor::render(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kDesignH);
    nvgFillColor(vg, rgb(col::ink));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kHeaderH);
    nvgFillColor(vg, rgb(col::panel));
    nvgFill(vg);
    hairline(vg, 0, kHeaderH, kDesignW, kHeaderH, rgb(col::edgeHi));
    label(vg, "Supple Audio", 30, 20, 10, rgb(col::dust));
    label(vg, "PROBE SYNTH", 30, 43, 24, rgb(col::bone));
    label(vg, "Eight-voice test instrument", 790, 40, 11, rgb(col::accent), Align::Right);
    for (const auto& g : kControls) {
        const ParamDef* p = findParam(g.id);
        char value[32];
        formatCompact(g.id, host_.paramValue(g.id), value, sizeof(value));
        knob(vg, g.x, g.y, g.r, static_cast<float>(toNormalised(*p, host_.paramValue(g.id))),
             p->name, value, p->unit, false,
             hover_ == static_cast<int>(g.id) || dragId_ == static_cast<int>(g.id));
    }
    label(vg, "CLAP instrument · sample-accurate notes", 30, kFooterTop + 1, 10, rgb(col::dim));
    char meter[48];
    std::snprintf(meter, sizeof(meter), "Output %+.1f dBFS", latestOut_);
    label(vg, meter, kDesignW - 30, kFooterTop + 1, 10, rgb(col::dim), Align::Right);
    drawFocus(vg);
    for (const auto& p : kParams) shown_[p.id] = host_.paramValue(p.id);
    dirty_ = false;
}

int Editor::controlAt(float x, float y) const {
    for (const auto& g : kControls) {
        const float dx = x - g.x, dy = y - g.y;
        if (dx * dx + dy * dy <= (g.r + 14) * (g.r + 14)) return static_cast<int>(g.id);
    }
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
    if (id == static_cast<int>(kWaveform) || id == static_cast<int>(kFilterSlope)) {
        host_.beginEdit(p->id);
        const double next = host_.paramValue(p->id) + 1.0;
        host_.setParam(p->id, next > p->max ? p->min : next);
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
    if (!p || p->readonly) return;
    if (dragId_ >= 0) onMouseUp();
    host_.beginEdit(p->id);
    host_.setParam(p->id, p->def);
    host_.endEdit(p->id);
    focusParam(p->id);
}

bool Editor::onScroll(float x, float y, float dy) {
    const int id = controlAt(x, y);
    const ParamDef* p = id >= 0 ? findParam(static_cast<uint32_t>(id)) : nullptr;
    if (!p || p->readonly) return false;
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
    if (const ParamDef* p = findParam(id))
        host_.setParam(
            id, p->stepped ? std::round(fromNormalised(*p, normal)) : fromNormalised(*p, normal));
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
    if (!p || p->readonly) return;
    if ((key == EditorKey::Enter || key == EditorKey::Space) && p->stepped) {
        host_.beginEdit(p->id);
        host_.setParam(p->id,
                       host_.paramValue(p->id) >= p->max ? p->min : host_.paramValue(p->id) + 1.0);
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
    setFromNormal(p->id, normal);
    host_.endEdit(p->id);
}

void Editor::drawFocus(NVGcontext* vg) {
    if (focus_ < 0) return;
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

}  // namespace probesynth::gui
