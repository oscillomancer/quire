// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char transient_font_mono[];
extern const unsigned int transient_font_mono_size;
extern const unsigned char transient_font_mono_medium[];
extern const unsigned int transient_font_mono_medium_size;
extern const unsigned char transient_font_display[];
extern const unsigned int transient_font_display_size;
extern const unsigned char transient_font_ui[];
extern const unsigned int transient_font_ui_size;
extern const unsigned char transient_font_ui_semibold[];
extern const unsigned int transient_font_ui_semibold_size;
}

namespace supple::transient::gui {
namespace {
constexpr uint32_t kEditable[] = {kAttack, kSustain, kSpeed, kSensitivity, kClip, kMix, kOutput};
struct ControlGeom {
    uint32_t id;
    float x, y, r;
};
constexpr ControlGeom kControls[] = {{kAttack, 105, 419, 45}, {kSustain, 240, 419, 45},
                                     {kSpeed, 390, 419, 30},  {kSensitivity, 490, 419, 30},
                                     {kClip, 600, 419, 30},   {kMix, 705, 419, 30},
                                     {kOutput, 815, 419, 30}};
float plotY(float db) { return 262 - std::clamp((db + 60) / 66.f, 0.f, 1.f) * 137; }
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(transient_font_mono),
                                   transient_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(transient_font_mono_medium),
                         transient_font_mono_medium_size, 0);
    const int c =
        nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(transient_font_display),
                         transient_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(transient_font_ui),
                                   transient_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(transient_font_ui_semibold),
                         transient_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    if (fontsReady_) {
        // The bundled Plex subsets omit infinity; Inter contains the glyph.
        nvgAddFallbackFontId(vg, a, e);
        nvgAddFallbackFontId(vg, b, e);
    }
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    if (dragId_ >= 0 && !controlEnabled(static_cast<uint32_t>(dragId_))) onMouseUp();
    if (scrollId_ >= 0 && !controlEnabled(static_cast<uint32_t>(scrollId_))) closeScrollGesture();
    if (focus_ >= 0 && !controlEnabled(static_cast<uint32_t>(focus_))) focus_ = -1;
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        latestGain_ = f.gain;
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
        historyFill_ += static_cast<float>(kHistoryColumns) / (20.0f * kHistorySeconds);
        while (historyFill_ >= 1.0f) {
            historyIn_[historyHead_] = latestIn_;
            historyGain_[historyHead_] = latestGain_;
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
    label(vg, "Supple Audio", 20, 19, 10, rgb(col::dust));
    label(vg, kName, 20, 43, 25, rgb(col::bone), Align::Left, true);
    label(vg, "TRANSIENT SHAPER", 880, 36, 11, rgb(col::accent), Align::Right, true);
    hairline(vg, 0, 62.5f, 900, 62.5f, rgb(col::edgeHi));
    cell(vg, 20, 80, 860, 232, "Input / output envelopes · last 2 seconds");
    for (int d : {-60, -48, -36, -24, -12, 0}) {
        const float y = plotY(static_cast<float>(d));
        hairline(vg, 65, y, 855, y, rgba(col::edgeHi, .5f));
        char text[16];
        std::snprintf(text, sizeof(text), "%d", d);
        num(vg, text, 55, y, 9, rgb(col::dim), Align::Right);
    }
    for (int trace = 0; trace < 3; ++trace) {
        nvgBeginPath(vg);
        for (int i = 0; i < kHistoryColumns; ++i) {
            const int j = (historyHead_ + i) % kHistoryColumns;
            const float x = 65 + 790.f * static_cast<float>(i) / (kHistoryColumns - 1);
            const float y = trace == 2 ? 193 - std::clamp(historyGain_[j], -12.f, 12.f) * 5
                                       : plotY(trace == 0 ? historyIn_[j] : historyOut_[j]);
            if (i == 0)
                nvgMoveTo(vg, x, y);
            else
                nvgLineTo(vg, x, y);
        }
        nvgStrokeColor(vg, trace == 0   ? rgba(col::cool, .75f)
                           : trace == 1 ? rgb(col::bone)
                                        : rgb(col::accent));
        nvgStrokeWidth(vg, trace == 2 ? 2.f : 1.4f);
        nvgStroke(vg);
    }
    label(vg, "INPUT", 70, 292, 10, rgb(col::cool));
    label(vg, "OUTPUT", 145, 292, 10, rgb(col::bone));
    label(vg, "GAIN ±12 dB", 230, 292, 10, rgb(col::accent));
    char peaks[120];
    std::snprintf(peaks, sizeof(peaks), "In %+.1f dBFS   Out %+.1f dBFS   Gain %+.1f dB", latestIn_,
                  latestOut_, latestGain_);
    num(vg, peaks, 855, 292, 10, rgb(col::dust), Align::Right);
    cell(vg, 20, 333, 290, 185, "Shape");
    cell(vg, 330, 333, 215, 185, "Detector");
    cell(vg, 565, 333, 315, 185, "Output");
    for (const auto& g : kControls) {
        const auto* p = findParam(g.id);
        const auto value = host_.paramValue(g.id);
        if (p->type == params::Type::Float) {
            const auto text = formatValue(g.id, value);
            knob(vg, g.x, g.y, g.r, static_cast<float>(toNormalised(*p, value)), p->name.data(),
                 text.c_str(), "", g.r > 40, hover_ == static_cast<int>(g.id),
                 g.id == kAttack || g.id == kSustain || g.id == kOutput);
        } else {
            label(vg, p->name.data(), g.x, g.y - 30, 10, rgb(col::dust), Align::Center);
            pill(vg, g.x - 30, g.y - 10, 60, 24, value > .5 ? "On" : "Off", value > .5,
                 rgb(col::accent), true, hover_ == static_cast<int>(g.id));
            label(vg, "2×", g.x, g.y + 28, 10, rgb(col::dim), Align::Center);
        }
    }
    label(vg, "CLAP", 20, 543, 9, rgb(col::dim));
    char latency[60];
    std::snprintf(latency, sizeof(latency), "Latency %d samples", host_.latencySamples());
    label(vg, latency, 880, 543, 9, rgb(col::dim), Align::Right);
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

bool Editor::controlEnabled(uint32_t id) const { return id < kInputLevel; }

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
        for (int i = 0; i < static_cast<int>(kInputLevel); ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        for (int visited = 0; visited < static_cast<int>(kInputLevel); ++visited) {
            index = index < 0 ? (fineOrReverse ? static_cast<int>(kInputLevel) - 1 : 0)
                              : (index + (fineOrReverse ? static_cast<int>(kInputLevel) - 1 : 1)) %
                                    static_cast<int>(kInputLevel);
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

}  // namespace supple::transient::gui
