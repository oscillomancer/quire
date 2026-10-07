// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char utility_font_mono[];
extern const unsigned int utility_font_mono_size;
extern const unsigned char utility_font_mono_medium[];
extern const unsigned int utility_font_mono_medium_size;
extern const unsigned char utility_font_display[];
extern const unsigned int utility_font_display_size;
extern const unsigned char utility_font_ui[];
extern const unsigned int utility_font_ui_size;
extern const unsigned char utility_font_ui_semibold[];
extern const unsigned int utility_font_ui_semibold_size;
}

namespace supple::utility::gui {
namespace {
constexpr uint32_t kEditable[] = {kGain,       kPan,         kWidth,     kChannels, kSwap,
                                  kInvertLeft, kInvertRight, kMonoBelow, kDcBlock,  kMute};
struct ControlGeom {
    uint32_t id;
    float x, y, r;
};
constexpr ControlGeom kControls[] = {{kGain, 62, 106, 25},         {kPan, 160, 106, 25},
                                     {kWidth, 338, 86, 25},        {kChannels, 338, 134, 25},
                                     {kSwap, 463, 134, 18},        {kInvertLeft, 262, 218, 18},
                                     {kInvertRight, 334, 218, 18}, {kDcBlock, 410, 218, 18},
                                     {kMute, 477, 218, 18},        {kMonoBelow, 75, 220, 22}};
// The compact panel keeps data below the dial, outside the pointer's entire sweep.
void utilityKnob(NVGcontext* vg, const ControlGeom& g, const ParamDef& p, double value, bool hot) {
    const float t = static_cast<float>(toNormalised(p, value));
    const float angle = kKnobA0 + (kKnobA1 - kKnobA0) * t;
    const float origin = g.id == kPan ? (kKnobA0 + kKnobA1) * 0.5f : kKnobA0;
    nvgBeginPath(vg);
    nvgCircle(vg, g.x, g.y + 3, g.r + 9);
    nvgFillPaint(vg, nvgRadialGradient(vg, g.x, g.y + 3, g.r - 7, g.r + 9, rgba(col::ink, 0.78f),
                                       rgba(col::ink, 0)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgArc(vg, g.x, g.y, g.r, kKnobA0, kKnobA1, NVG_CW);
    nvgStrokeColor(vg, rgb(col::knobWell));
    nvgStrokeWidth(vg, 5);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
    nvgStrokeColor(vg, rgba(col::edgeHi, 0.3f));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    if (std::abs(angle - origin) > 0.001f) {
        nvgBeginPath(vg);
        nvgArc(vg, g.x, g.y, g.r, std::min(angle, origin), std::max(angle, origin), NVG_CW);
        glowStroke(vg, rgb(col::accent), hot ? 3.2f : 2.8f, 2);
    }
    nvgBeginPath(vg);
    nvgCircle(vg, g.x, g.y, g.r - 5);
    nvgFillPaint(vg, nvgLinearGradient(vg, g.x, g.y - g.r, g.x, g.y + g.r, rgb(col::knobTop),
                                       rgb(col::knobBot)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(col::knobRim, hot ? 1.0f : 0.75f));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    const float px = g.x + std::cos(angle) * (g.r - 11);
    const float py = g.y + std::sin(angle) * (g.r - 11);
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, 3.8f);
    nvgFillColor(vg, rgba(col::well, 0.85f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, hot ? 2.8f : 2.45f);
    nvgFillColor(vg, rgb(col::bone));
    nvgFill(vg);
    label(vg, p.name.data(), g.x, g.y + g.r + 13, 10.5f, rgb(col::dust), Align::Center, true);
    const auto text = pa::format(p, value);
    num(vg, text.c_str(), g.x, g.y + g.r + 26, 10, rgb(col::bone), Align::Center);
}
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(utility_font_mono),
                                   utility_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(utility_font_mono_medium),
                         utility_font_mono_medium_size, 0);
    const int c =
        nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(utility_font_display),
                         utility_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(utility_font_ui),
                                   utility_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(utility_font_ui_semibold),
                         utility_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        latestCorrelation_ = f.correlation;
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
    label(vg, "Supple Audio", 20, 18, 10, rgb(col::dust));
    label(vg, kDisplayName, 20, 40, 22, rgb(col::bone));
    label(vg, "STEREO UTILITY", 500, 36, 10, rgb(col::dust), Align::Right);
    for (const auto& g : kControls) {
        const auto* p = findParam(g.id);
        const double v = host_.paramValue(g.id);
        char value[48];
        formatCompact(g.id, v, value, sizeof(value));
        if (g.id == kGain || g.id == kPan || g.id == kMonoBelow) {
            utilityKnob(vg, g, *p, v,
                        hover_ == static_cast<int>(g.id) || dragId_ == static_cast<int>(g.id));
        } else if (g.id == kWidth) {
            label(vg, p->name.data(), 242, 70, 11, rgb(col::dust));
            const auto widthText = pa::format(*p, v);
            label(vg, widthText.c_str(), 410, 70, 11, rgb(col::accent), Align::Right);
            hairline(vg, 242, 86, 414, 86, rgb(col::edgeHi));
            const float x = 242 + 172 * static_cast<float>(toNormalised(*p, v));
            nvgBeginPath(vg);
            nvgCircle(vg, x, 86, 5);
            nvgFillColor(vg, rgb(col::accent));
            nvgFill(vg);
        } else {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, g.x - g.r - 12, g.y - 15, 2 * g.r + 24, 30, 4);
            nvgFillColor(vg, rgb(v != p->def ? col::edgeHi : col::panel));
            nvgFill(vg);
            label(vg, g.id == kChannels ? value : p->name.data(), g.x, g.y + 4, 10,
                  rgb(v != p->def ? col::accent : col::dust), Align::Center);
        }
    }
    char meter[96];
    std::snprintf(meter, sizeof(meter), "In %+.1f  Out %+.1f dBFS    Corr %+.2f", latestIn_,
                  latestOut_, latestCorrelation_);
    label(vg, meter, 20, 286, 10, rgb(col::dust));
    hairline(vg, 20, 166, 500, 166, rgb(col::edge));
    label(vg, "POLARITY / CLEANUP", 225, 188, 10, rgb(col::dim));
    drawFocus(vg);
    for (const auto& p : kParams) shown_[p.id] = host_.paramValue(p.id);
    dirty_ = false;
}

int Editor::controlAt(float x, float y) const {
    if (x >= 242 && x <= 414 && y >= 74 && y <= 98) return kWidth;
    for (const auto& g : kControls) {
        if (g.id == kWidth) continue;
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
    if (p->type != pa::Type::Float) {
        host_.beginEdit(p->id);
        const double next = host_.paramValue(p->id) + 1.0;
        host_.setParam(p->id, next > p->max ? p->min : next);
        host_.endEdit(p->id);
    } else {
        dragId_ = id;
        dragStartX_ = x;
        dragStartY_ = y;
        dragStartNormal_ = toNormalised(*p, host_.paramValue(p->id));
        host_.beginEdit(p->id);
        if (id == kWidth) {
            setFromNormal(kWidth, (x - 242) / 172.0);
            dragStartNormal_ = toNormalised(*p, host_.paramValue(p->id));
        }
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
    if (dragId_ == kWidth) {
        setFromNormal(kWidth, dragStartNormal_ + (x - dragStartX_) * (fine ? 0.25 : 1.0) / 172.0);
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
        host_.setParam(id, (p->type != pa::Type::Float) ? std::round(fromNormalised(*p, normal))
                                                        : fromNormalised(*p, normal));
    dirty_ = true;
}

void Editor::onKey(EditorKey key, bool fineOrReverse) {
    if (key == EditorKey::Tab) {
        int index = 0;
        for (int i = 0; i < 10; ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        index = focus_ < 0 ? (fineOrReverse ? 9 : 0) : (index + (fineOrReverse ? 9 : 1)) % 10;
        focusParam(kEditable[index]);
        return;
    }
    if (focus_ < 0) return;
    const ParamDef* p = findParam(static_cast<uint32_t>(focus_));
    if (!p || (p->flags & pa::kReadonly)) return;
    if ((key == EditorKey::Enter || key == EditorKey::Space) && (p->type != pa::Type::Float)) {
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
    if (p->type != pa::Type::Float && key != EditorKey::Home && key != EditorKey::End)
        host_.setParam(p->id,
                       std::clamp(host_.paramValue(p->id) +
                                      (key == EditorKey::Left || key == EditorKey::Down ? -1. : 1.),
                                  p->min, p->max));
    else
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

}  // namespace supple::utility::gui
