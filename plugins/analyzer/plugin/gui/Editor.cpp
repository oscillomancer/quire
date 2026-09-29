// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"

extern "C" {
extern const unsigned char analyzer_font_mono[];
extern const unsigned int analyzer_font_mono_size;
extern const unsigned char analyzer_font_mono_medium[];
extern const unsigned int analyzer_font_mono_medium_size;
extern const unsigned char analyzer_font_display[];
extern const unsigned int analyzer_font_display_size;
extern const unsigned char analyzer_font_ui[];
extern const unsigned int analyzer_font_ui_size;
extern const unsigned char analyzer_font_ui_semibold[];
extern const unsigned int analyzer_font_ui_semibold_size;
}

namespace analyzer::gui {
namespace {
constexpr uint32_t kEditable[] = {kWindow, kHold, kReset};
struct ControlGeom {
    uint32_t id;
    float x, y, w;
};
constexpr ControlGeom kControls[] = {
    {kWindow, 30, 477, 190}, {kHold, 250, 477, 100}, {kReset, 380, 477, 100}};
float historyY(float lu) { return 372 - std::clamp((lu + 60) / 60, 0.0f, 1.0f) * 145; }
}  // namespace

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(analyzer_font_mono),
                                   analyzer_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(analyzer_font_mono_medium),
                         analyzer_font_mono_medium_size, 0);
    const int c =
        nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(analyzer_font_display),
                         analyzer_font_display_size, 0);
    const int d = nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(analyzer_font_ui),
                                   analyzer_font_ui_size, 0);
    const int e =
        nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(analyzer_font_ui_semibold),
                         analyzer_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    return fontsReady_;
}

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    Frame f;
    while (host_.telemetry().pop(f)) {
        if (f.analysis.elapsedSeconds < latest_.elapsedSeconds) {
            std::fill(std::begin(history_), std::end(history_), -120.0f);
            historyElapsed_ = 0;
        }
        historyElapsed_ += std::max(0.0, f.analysis.elapsedSeconds - latest_.elapsedSeconds);
        latest_ = f.analysis;
        while (historyElapsed_ >= kHistorySeconds / kHistoryColumns) {
            history_[historyHead_] = static_cast<float>(latest_.measurements.shortTerm);
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
    label(vg, "Loudness · spectrum · stereo", 790, 40, 11, rgb(col::accent), Align::Right);
    const auto& m = latest_.measurements;
    const uint32_t meters[]{kMomentary, kShortTerm, kIntegrated, kLra, kTruePeak};
    const double values[]{m.momentary, m.shortTerm, m.integrated, m.lra, m.truePeak};
    for (int i = 0; i < 5; ++i) {
        const float x = 30 + static_cast<float>(i) * 155;
        cell(vg, x, 82, 140, 98, table()[meters[i]].name.data());
        const std::string number =
            values[i] <= -120 ? "—" : supple::params::formatCompact(table()[meters[i]], values[i]);
        num(vg, number.c_str(), x + 12, 133, 30, rgb(i == 2 ? col::accent : col::bone));
        label(vg, i == 4 ? "dBTP" : supple::units::unitName(table()[meters[i]].unit).data(), x + 12,
              162, 10, rgb(col::dust));
    }
    cell(vg, 30, 196, 350, 192, "Short-term history · LUFS · 10 s");
    for (int lu : {-60, -40, -20, 0}) {
        const float y = historyY(static_cast<float>(lu));
        hairline(vg, 65, y, 362, y, rgba(col::edgeHi, .6f));
        char text[12];
        std::snprintf(text, sizeof(text), "%d", lu);
        num(vg, text, 57, y, 9, rgb(col::dim), Align::Right);
    }
    nvgBeginPath(vg);
    for (int i = 0; i < kHistoryColumns; ++i) {
        const float x = 65 + 297 * static_cast<float>(i) / (kHistoryColumns - 1),
                    y = historyY(history_[(historyHead_ + i) % kHistoryColumns]);
        if (i == 0)
            nvgMoveTo(vg, x, y);
        else
            nvgLineTo(vg, x, y);
    }
    nvgStrokeColor(vg, rgb(col::accent));
    nvgStrokeWidth(vg, 1.8f);
    nvgStroke(vg);
    cell(vg, 400, 196, 390, 192, "1/3-octave spectrum · mid · dBFS");
    for (int db : {-90, -60, -30, 0}) {
        const float y = 371 - static_cast<float>(db + 90) / 90 * 142;
        hairline(vg, 429, y, 775, y, rgba(col::edgeHi, .6f));
        char text[12];
        std::snprintf(text, sizeof(text), "%d", db);
        num(vg, text, 423, y, 9, rgb(col::dim), Align::Right);
    }
    for (int i = 0; i < 31; ++i) {
        const float x = 430 + static_cast<float>(i) * 11.1f,
                    h = std::clamp(
                            static_cast<float>((m.spectrum[static_cast<std::size_t>(i)] + 90) / 90),
                            0.0f, 1.0f) *
                        142;
        nvgBeginPath(vg);
        nvgRect(vg, x, 371 - h, 7, h);
        nvgFillColor(vg, rgba(col::cool, .85f));
        nvgFill(vg);
    }
    label(vg, "20 Hz", 430, 380, 9, rgb(col::dust));
    label(vg, "1 kHz", 619, 380, 9, rgb(col::dust));
    label(vg, "20 kHz", 776, 380, 9, rgb(col::dust), Align::Right);
    const auto bar = [&](float x, const char* title, double value, double lo, double hi,
                         const char* text) {
        label(vg, title, x, 416, 10, rgb(col::dust));
        num(vg, text, x + 350, 416, 11, rgb(col::bone), Align::Right);
        nvgBeginPath(vg);
        nvgRect(vg, x, 433, 350, 5);
        nvgFillColor(vg, rgb(col::edgeHi));
        nvgFill(vg);
        const float pos =
            x + 350 * static_cast<float>(std::clamp((value - lo) / (hi - lo), 0.0, 1.0));
        hairline(vg, pos, 429, pos, 442, rgb(col::accent), 2);
    };
    const auto corr = supple::params::formatCompact(table()[kCorrelation], m.correlation);
    const auto bal = supple::params::format(table()[kBalance], m.balance);
    bar(30, table()[kCorrelation].name.data(), m.correlation, -1, 1, corr.c_str());
    bar(430, table()[kBalance].name.data(), m.balance, -24, 24, bal.c_str());
    for (const auto& g : kControls) {
        const auto* p = findParam(g.id);
        const auto text = supple::params::format(*p, host_.paramValue(g.id));
        std::string caption = std::string(p->name) + " " + (g.id == kWindow ? text : "");
        pill(vg, g.x, g.y, g.w, 26, caption.c_str(), host_.paramValue(g.id) != 0, rgb(col::accent),
             true, hover_ == static_cast<int>(g.id));
    }
    char extra[80];
    std::snprintf(extra, sizeof(extra), "Width %.0f %% · centroid %.0f Hz", m.width * 100,
                  m.centroid);
    label(vg, extra, 790, 489, 10, rgb(col::dust), Align::Right);
    label(vg, "Bit-exact pass-through · 0 samples latency", 30, kFooterTop, 10, rgb(col::dim));
    label(vg,
          latest_.droppedFrames ? "Measurement gap — history restarted"
          : latest_.held        ? "Measurements held"
                                : "Live analysis",
          790, kFooterTop, 10, rgb(latest_.droppedFrames ? col::alert : col::dim), Align::Right);
    drawFocus(vg);
    for (const auto& p : kParams) shown_[p.id] = host_.paramValue(p.id);
    dirty_ = false;
}

int Editor::controlAt(float x, float y) const {
    for (const auto& g : kControls)
        if (x >= g.x && x <= g.x + g.w && y >= g.y && y <= g.y + 26) return static_cast<int>(g.id);
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
        host_.setParam(p->id, p->id == kReset                     ? 1
                              : host_.paramValue(p->id) >= p->max ? p->min
                                                                  : host_.paramValue(p->id) + 1);
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
    host_.setParam(p->id, std::clamp(host_.paramValue(p->id) + (dy < 0 ? 1 : -1), p->min, p->max));
    return true;
}

void Editor::closeScrollGesture() {
    if (scrollId_ >= 0) host_.endEdit(static_cast<uint32_t>(scrollId_));
    scrollId_ = -1;
    scrollIdle_ = 0.0f;
}

void Editor::setFromNormal(uint32_t id, double normal) {
    if (const ParamDef* p = findParam(id))
        host_.setParam(id, std::round(fromNormalised(*p, normal)));
    dirty_ = true;
}

void Editor::onKey(EditorKey key, bool fineOrReverse) {
    if (key == EditorKey::Tab) {
        int index = 0;
        for (int i = 0; i < 3; ++i)
            if (static_cast<int>(kEditable[i]) == focus_) index = i;
        index = focus_ < 0 ? (fineOrReverse ? 2 : 0) : (index + (fineOrReverse ? 2 : 1)) % 3;
        focusParam(kEditable[index]);
        return;
    }
    if (focus_ < 0) return;
    const ParamDef* p = findParam(static_cast<uint32_t>(focus_));
    if (!p || (p->flags & pa::kReadonly)) return;
    if ((key == EditorKey::Enter || key == EditorKey::Space) && p->type != pa::Type::Float) {
        host_.beginEdit(p->id);
        host_.setParam(p->id, p->id == kReset                     ? 1
                              : host_.paramValue(p->id) >= p->max ? p->min
                                                                  : host_.paramValue(p->id) + 1);
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
        normal -= 1 / (p->max - p->min);
    else if (key == EditorKey::Right || key == EditorKey::Up)
        normal += 1 / (p->max - p->min);
    else
        return;
    host_.beginEdit(p->id);
    setFromNormal(p->id, normal);
    host_.endEdit(p->id);
}

void Editor::drawFocus(NVGcontext* vg) {
    for (const auto& g : kControls)
        if (focus_ == static_cast<int>(g.id))
            strokeRoundRect(vg, g.x - 3, g.y - 3, g.w + 6, 32, 5, rgba(col::bone, .7f));
}

void Editor::onFocusLost() {
    onMouseUp();
    closeScrollGesture();
    focus_ = -1;
    hover_ = -1;
    dirty_ = true;
}

}  // namespace analyzer::gui
