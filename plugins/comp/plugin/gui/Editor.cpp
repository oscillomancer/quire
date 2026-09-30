// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char comp_font_mono[];
extern const unsigned int comp_font_mono_size;
extern const unsigned char comp_font_mono_medium[];
extern const unsigned int comp_font_mono_medium_size;
extern const unsigned char comp_font_display[];
extern const unsigned int comp_font_display_size;
extern const unsigned char comp_font_ui[];
extern const unsigned int comp_font_ui_size;
extern const unsigned char comp_font_ui_semibold[];
extern const unsigned int comp_font_ui_semibold_size;
}

namespace supple::comp::gui {
namespace {
constexpr float kGraphX = 20, kGraphY = 80, kGraphW = 430, kGraphH = 200;
constexpr uint32_t kEditable[] = {kMode,      kThreshold,   kRatio,     kKnee,       kAttack,
                                  kRelease,   kAutoRelease, kDetector,  kRmsWindow,  kTopology,
                                  kLookahead, kTruePeak,    kSidechain, kScHighpass, kScLowpass,
                                  kScListen,  kStereoLink,  kMakeup,    kAutoMakeup, kCeiling,
                                  kMix,       kOutput};
struct ControlGeom {
    uint32_t id;
    float x, y, r;
};
constexpr ControlGeom kControls[] = {
    {kThreshold, 55, 358, 22},   {kRatio, 125, 358, 22},     {kKnee, 195, 358, 22},
    {kAttack, 265, 358, 22},     {kRelease, 335, 358, 22},   {kAutoRelease, 405, 358, 22},
    {kDetector, 55, 460, 22},    {kRmsWindow, 125, 460, 22}, {kTopology, 195, 460, 22},
    {kLookahead, 265, 460, 22},  {kTruePeak, 335, 460, 22},  {kSidechain, 505, 358, 22},
    {kScHighpass, 575, 358, 22}, {kScLowpass, 645, 358, 22}, {kScListen, 505, 460, 22},
    {kStereoLink, 575, 460, 22}, {kMakeup, 735, 358, 22},    {kAutoMakeup, 795, 358, 22},
    {kCeiling, 855, 358, 22},    {kMix, 765, 460, 22},       {kOutput, 835, 460, 22},
};
float plotY(float db) { return 249 - std::clamp((db + 60) / 60.f, 0.f, 1.f) * 142; }
float plotX(float db) { return 65 + std::clamp((db + 60) / 60.f, 0.f, 1.f) * 365; }
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(comp_font_mono),
                                   comp_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(comp_font_mono_medium),
                         comp_font_mono_medium_size, 0);
    const int c = nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(comp_font_display),
                                   comp_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(comp_font_ui),
                                   comp_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(comp_font_ui_semibold),
                         comp_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    if (fontsReady_) {
        // The bundled Plex subsets omit infinity; Inter contains the glyph.
        nvgAddFallbackFontId(vg, a, e);
        nvgAddFallbackFontId(vg, b, e);
    }
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        latestGr_ = f.gr;
        latestDetector_ = f.detectorDb;
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
            historyIn_[historyHead_] = latestGr_;
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
    const bool limiter = host_.paramValue(kMode) > .5;
    pill(vg, 190, 22, 125, 26, limiter ? "Limiter" : "Compressor", true, rgb(col::accent), true,
         hover_ == kMode);
    label(vg, "DYNAMICS", 880, 36, 11, rgb(col::accent), Align::Right, true);
    hairline(vg, 0, 62.5f, 900, 62.5f, rgb(col::edgeHi));
    cell(vg, kGraphX, kGraphY, kGraphW, kGraphH,
         limiter ? "Limiter transfer · dBFS" : "Transfer · input / output dBFS");
    for (int d : {-60, -48, -36, -24, -12, 0}) {
        const float x = plotX(static_cast<float>(d)), y = plotY(static_cast<float>(d));
        hairline(vg, x, 107, x, 249, rgba(col::edgeHi, .4f));
        hairline(vg, 65, y, 430, y, rgba(col::edgeHi, .4f));
        char t[12];
        std::snprintf(t, sizeof(t), "%d", d);
        num(vg, t, x, 265, 9, rgb(col::dust), Align::Center);
        num(vg, t, 55, y, 9, rgb(col::dust), Align::Right);
    }
    hairline(vg, 65, 249, 430, 107, rgba(col::dust, .3f));
    auto transfer = [&](double in) {
        if (limiter) return std::min(in, host_.paramValue(kCeiling));
        return in + gainReduction(in, host_.paramValue(kThreshold), host_.paramValue(kRatio),
                                  host_.paramValue(kKnee));
    };
    nvgBeginPath(vg);
    for (int i = 0; i <= 240; ++i) {
        const double input = -60 + i * .25;
        const float x = plotX(static_cast<float>(input)),
                    y = plotY(static_cast<float>(transfer(input)));
        if (i == 0)
            nvgMoveTo(vg, x, y);
        else
            nvgLineTo(vg, x, y);
    }
    nvgStrokeWidth(vg, 2);
    nvgStrokeColor(vg, rgb(col::accent));
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, plotX(latestDetector_), plotY(static_cast<float>(transfer(latestDetector_))), 4);
    nvgFillColor(vg, rgb(col::bone));
    nvgFill(vg);
    cell(vg, 470, 80, 340, 170, "Gain reduction · last 4 seconds");
    for (int d : {0, -6, -12, -18, -24}) {
        float y = 110 - static_cast<float>(d) * 5;
        hairline(vg, 490, y, 790, y, rgba(col::edgeHi, .5f));
        char text[10];
        std::snprintf(text, sizeof(text), "%d", d);
        num(vg, text, 785, y - 6, 9, rgb(col::dim), Align::Right);
    }
    nvgBeginPath(vg);
    for (int i = 0; i < kHistoryColumns; ++i) {
        const float x = 490 + 300.f * static_cast<float>(i) / (kHistoryColumns - 1),
                    y = 110 -
                        std::clamp(historyIn_[(historyHead_ + i) % kHistoryColumns], -24.f, 0.f) *
                            5;
        if (i == 0)
            nvgMoveTo(vg, x, y);
        else
            nvgLineTo(vg, x, y);
    }
    nvgStrokeColor(vg, rgb(col::accent));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
    cell(vg, 830, 80, 50, 170, "GR");
    nvgBeginPath(vg);
    nvgRect(vg, 846, 112, 18, std::clamp(-latestGr_, 0.f, 24.f) * 4.4f);
    nvgFillColor(vg, rgb(col::accent));
    nvgFill(vg);
    char gr[24];
    std::snprintf(gr, sizeof(gr), "%.1f", latestGr_);
    num(vg, gr, 855, 232, 12, rgb(col::bone), Align::Center);
    if (limiter)
        for (int c = 0; c < 2; ++c) {
            const float x = c == 0 ? 485.f : 675.f;
            const float peak = c == 0 ? latestIn_ : latestOut_;
            hairline(vg, x, 256, x + 130, 256, rgba(col::edgeHi, .8f));
            hairline(vg, x, 256, x + 130 * std::clamp((peak + 60) / 66.f, 0.f, 1.f), 256,
                     rgb(col::cool));
            const float ceilingX =
                x + 130 * static_cast<float>((host_.paramValue(kCeiling) + 60) / 66);
            hairline(vg, ceilingX, 252, ceilingX, 260, rgb(col::accent));
        }
    char peaks[100];
    std::snprintf(peaks, sizeof(peaks), "Input %+.1f dBFS     Output %+.1f dBFS", latestIn_,
                  latestOut_);
    label(vg, peaks, 475, 269, 11, rgb(col::dust));
    cell(vg, 20, 300, 430, 225, "Detector");
    cell(vg, 470, 300, 210, 225, "Sidechain");
    cell(vg, 700, 300, 180, 225, "Output");
    for (const auto& g : kControls) {
        const auto* p = findParam(g.id);
        const double value = host_.paramValue(g.id);
        char text[48];
        formatCompact(g.id, value, text, sizeof(text));
        if (p->type == params::Type::Float) {
            if (g.id == kRatio && value >= 100) std::snprintf(text, sizeof(text), "∞");
            knob(vg, g.x, g.y, g.r, static_cast<float>(toNormalised(*p, value)), p->name.data(),
                 text, units::unitName(p->unit).data(), false, hover_ == static_cast<int>(g.id));
        } else {
            label(vg, p->name.data(), g.x, g.y - 26, 9, rgb(col::dust), Align::Center);
            const auto formatted = params::format(*p, value);
            const bool boolean = p->type == params::Type::Bool;
            const float width = boolean ? 56.f : 64.f;
            pill(vg, g.x - width / 2, g.y - 10, width, 22, formatted.c_str(), value > .5,
                 rgb(col::accent), boolean, hover_ == static_cast<int>(g.id));
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
        const float dx = x - g.x, dy = y - g.y;
        if (dx * dx + dy * dy <= (g.r + 14) * (g.r + 14)) return static_cast<int>(g.id);
    }
    if (x >= 190 && x <= 315 && y >= 22 && y <= 48) return kMode;
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
        int index = 0;
        for (int i = 0; i < kGr; ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        index = focus_ < 0 ? (fineOrReverse ? kGr - 1 : 0)
                           : (index + (fineOrReverse ? kGr - 1 : 1)) % kGr;
        focusParam(kEditable[index]);
        return;
    }
    if (focus_ < 0) return;
    const ParamDef* p = findParam(static_cast<uint32_t>(focus_));
    if (!p || (p->flags & params::kReadonly)) return;
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
    if (focus_ < 0) return;
    for (const auto& g : kControls)
        if (static_cast<int>(g.id) == focus_) {
            nvgBeginPath(vg);
            nvgCircle(vg, g.x, g.y, g.r + 8);
            nvgStrokeColor(vg, rgba(col::bone, 0.7f));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
    if (focus_ == static_cast<int>(kMode))
        strokeRoundRect(vg, 188, 20, 129, 30, 5, rgba(col::bone, 0.7f));
}

void Editor::onFocusLost() {
    onMouseUp();
    closeScrollGesture();
    focus_ = -1;
    hover_ = -1;
    dirty_ = true;
}

}  // namespace supple::comp::gui
