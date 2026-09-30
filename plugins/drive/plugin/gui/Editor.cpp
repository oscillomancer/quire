// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"
#include "drive/processor.h"

extern "C" {
extern const unsigned char drive_font_mono[];
extern const unsigned int drive_font_mono_size;
extern const unsigned char drive_font_mono_medium[];
extern const unsigned int drive_font_mono_medium_size;
extern const unsigned char drive_font_display[];
extern const unsigned int drive_font_display_size;
extern const unsigned char drive_font_ui[];
extern const unsigned int drive_font_ui_size;
extern const unsigned char drive_font_ui_semibold[];
extern const unsigned int drive_font_ui_semibold_size;
}

namespace supple::drive::gui {
namespace {
constexpr uint32_t kEditable[] = {kDrive,        kCurve,    kBias, kTone,
                                  kOversampling, kAutoGain, kMix,  kOutput};
struct ControlGeom {
    uint32_t id;
    float x, y, r;
};
constexpr ControlGeom kControls[] = {{kDrive, 90, 345, 40},         {kBias, 240, 345, 28},
                                     {kTone, 370, 345, 28},         {kMix, 500, 345, 28},
                                     {kOutput, 650, 345, 28},       {kCurve, 180, 282, 30},
                                     {kOversampling, 490, 282, 30}, {kAutoGain, 665, 282, 30}};
// The compact panel keeps data below the dial, outside the pointer's entire sweep.
void driveKnob(NVGcontext* vg, const ControlGeom& g, const ParamDef& p, double value, bool hot) {
    const float t = static_cast<float>(toNormalised(p, value));
    const float angle = kKnobA0 + (kKnobA1 - kKnobA0) * t;
    const float origin =
        g.id == kBias || g.id == kTone || g.id == kOutput ? (kKnobA0 + kKnobA1) * 0.5f : kKnobA0;
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
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(drive_font_mono),
                                   drive_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(drive_font_mono_medium),
                         drive_font_mono_medium_size, 0);
    const int c = nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(drive_font_display),
                                   drive_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(drive_font_ui),
                                   drive_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(drive_font_ui_semibold),
                         drive_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, .25));
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
    label(vg, "Supple Audio", 24, 18, 10, rgb(col::dim));
    label(vg, kDisplayName, 24, 40, 22, rgb(col::bone), Align::Left, true);
    label(vg, "SATURATION", 736, 36, 10, rgb(col::dust), Align::Right);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 24, 58);
    nvgLineTo(vg, 736, 58);
    nvgStrokeColor(vg, rgb(col::edge));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    label(vg, "TRANSFER", 30, 79, 10, rgb(col::dust), Align::Left, true);
    label(vg, "STATIC HARMONICS · -18 dBFS RMS", 415, 79, 10, rgb(col::dust), Align::Left, true);
    const int curve = static_cast<int>(host_.paramValue(kCurve));
    const double drive = std::pow(10.0, host_.paramValue(kDrive) / 20),
                 bias = host_.paramValue(kBias) / 100;
    auto transfer = [&](double x) {
        return curveValue(curve, x * drive + bias) - curveValue(curve, bias);
    };
    auto graph = [&](float x, float y, float w, float h) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w, h, 5);
        nvgFillColor(vg, rgb(col::well));
        nvgFill(vg);
        nvgStrokeColor(vg, rgb(col::edge));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
    };
    graph(30, 90, 335, 166);
    graph(415, 90, 315, 166);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 30, 173);
    nvgLineTo(vg, 365, 173);
    nvgMoveTo(vg, 197.5f, 90);
    nvgLineTo(vg, 197.5f, 256);
    nvgStrokeColor(vg, rgb(col::edge));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    nvgSave(vg);
    nvgScissor(vg, 30, 90, 335, 166);
    nvgBeginPath(vg);
    for (int i = 0; i <= 256; ++i) {
        const double x = -2 + 4.0 * i / 256;
        const float px = 30 + 335.0f * i / 256, py = 173 - static_cast<float>(transfer(x)) * 38;
        if (i == 0)
            nvgMoveTo(vg, px, py);
        else
            nvgLineTo(vg, px, py);
    }
    glowStroke(vg, rgb(col::accent), 2, 2);
    const double level = std::min(2.0, std::pow(10.0, latestIn_ / 20));
    nvgBeginPath(vg);
    nvgCircle(vg, 197.5f + static_cast<float>(level) * 83.75f,
              173 - static_cast<float>(transfer(level)) * 38, 3);
    nvgFillColor(vg, rgb(col::bone));
    nvgFill(vg);
    nvgRestore(vg);
    // A deterministic main-thread test sine; bars describe the static setting,
    // not an FFT of the programme signal arriving on the audio thread.
    for (int harmonic = 1; harmonic <= 12; ++harmonic) {
        double re = 0, im = 0;
        for (int i = 0; i < 1024; ++i) {
            const double phase = 2 * 3.141592653589793 * i / 1024;
            const double y =
                transfer(std::sqrt(2.0) * std::pow(10.0, -18.0 / 20) * std::sin(phase));
            re += y * std::cos(harmonic * phase);
            im -= y * std::sin(harmonic * phase);
        }
        const double db = 20 * std::log10(std::max(1e-9, 2 * std::hypot(re, im) / 1024));
        const float height = static_cast<float>(std::clamp((db + 80) / 80, 0.0, 1.0)) * 135;
        const float x = 429 + (harmonic - 1) * 24;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, 239 - height, 13, height, 2);
        nvgFillColor(vg, rgba(col::accent, harmonic == 1 ? 1 : .65f));
        nvgFill(vg);
        char numText[8];
        std::snprintf(numText, sizeof(numText), "%d", harmonic);
        num(vg, numText, x + 6, 248, 8, rgb(col::dim), Align::Center);
    }
    for (uint32_t id : {kCurve, kOversampling, kAutoGain}) {
        const float x = id == kCurve ? 30 : id == kOversampling ? 405 : 605;
        const float width = id == kCurve ? 335 : id == kOversampling ? 165 : 125;
        graph(x, 269, width, 27);
        const auto text = pa::format(kParams[id], host_.paramValue(id));
        label(vg, kParams[id].name.data(), x + 8, 282, 10, rgb(col::dust));
        label(vg, text.c_str(), x + width - 8, 282, 10, rgb(col::accent), Align::Right, true);
    }
    for (const auto& g : kControls)
        if (kParams[g.id].type == pa::Type::Float)
            driveKnob(vg, g, kParams[g.id], host_.paramValue(g.id),
                      hover_ == static_cast<int>(g.id));
    label(vg, "dB/oct", 370, 412, 9, rgb(col::dim), Align::Center);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 24, 430);
    nvgLineTo(vg, 736, 430);
    nvgStrokeColor(vg, rgb(col::edge));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    char status[120];
    std::snprintf(status, sizeof(status), "IN %.1f dBFS   OUT %.1f dBFS   LATENCY %d samples",
                  latestIn_, latestOut_, host_.latencySamples());
    num(vg, status, 30, 440, 9, rgb(col::dim));
    drawFocus(vg);
    for (const auto& p : kParams) shown_[p.id] = host_.paramValue(p.id);
    dirty_ = false;
}

int Editor::controlAt(float x, float y) const {
    if (y >= 269 && y <= 296) {
        if (x >= 30 && x <= 365) return kCurve;
        if (x >= 405 && x <= 570) return kOversampling;
        if (x >= 605 && x <= 730) return kAutoGain;
    }
    for (const auto& g : kControls) {
        if (kParams[g.id].type != pa::Type::Float) continue;
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
    if (dragId_ >= 0) onMouseUp();
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
    if (dragId_ >= 0) onMouseUp();
    if (scrollId_ != id) {
        closeScrollGesture();
        scrollId_ = id;
        host_.beginEdit(p->id);
    }
    scrollIdle_ = 0.0f;
    focusParam(p->id);
    if (p->type == pa::Type::Float)
        setFromNormal(p->id, toNormalised(*p, host_.paramValue(p->id)) - dy * .025);
    else if (dy != 0)
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
    if (const ParamDef* p = findParam(id))
        host_.setParam(id, (p->type != pa::Type::Float) ? std::round(fromNormalised(*p, normal))
                                                        : fromNormalised(*p, normal));
    dirty_ = true;
}

void Editor::onKey(EditorKey key, bool fineOrReverse) {
    if (key == EditorKey::Tab) {
        int index = 0;
        for (int i = 0; i < 8; ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        index = focus_ < 0 ? (fineOrReverse ? 7 : 0) : (index + (fineOrReverse ? 7 : 1)) % 8;
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
    if (p->type != pa::Type::Float && (key == EditorKey::Left || key == EditorKey::Down ||
                                       key == EditorKey::Right || key == EditorKey::Up)) {
        host_.beginEdit(p->id);
        host_.setParam(p->id,
                       std::clamp(host_.paramValue(p->id) +
                                      (key == EditorKey::Left || key == EditorKey::Down ? -1 : 1),
                                  p->min, p->max));
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
            if (kParams[g.id].type == pa::Type::Float)
                nvgCircle(vg, g.x, g.y, g.r + 8);
            else {
                const float x = g.id == kCurve ? 30 : g.id == kOversampling ? 405 : 605;
                const float width = g.id == kCurve ? 335 : g.id == kOversampling ? 165 : 125;
                nvgRoundedRect(vg, x - 2, 267, width + 4, 31, 5);
            }

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

}  // namespace supple::drive::gui
