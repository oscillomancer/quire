// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

#include "core/Dsp.h"
#include "core/ParamTable.h"
#include "gui/Widgets.h"
#include "Params.h"
#include "Processor.h"
#include "ProductInfo.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;
namespace dsp = supple::dsp;

namespace {

constexpr Rect kHeader{0, 0, kEditorWidth, 60};
constexpr Rect kGraphPanel{24, 76, 600, 300};
constexpr Rect kFilterPanel{24, 392, 600, 144};
constexpr Rect kLevelPanel{640, 76, 236, 460};
constexpr double kMinHz = 20.0, kMaxHz = 20000.0;
constexpr double kTopDb = 12.0, kBottomDb = -48.0;

}  // namespace

// The filter section exactly as Processor builds it, so the curve on screen
// is the response the audio has.
struct Editor::Response {
    dsp::Svf low2, low4a, low4b, high2, high4a, high4b;
    double sampleRate, input, output, mix, low, high, steep;

    double db(double hz) const {
        using C = std::complex<double>;
        const C hp2 = low2.highpassResponse(hz, sampleRate);
        const C hp4 = low4a.highpassResponse(hz, sampleRate) * low4b.highpassResponse(hz, sampleRate);
        const C lp2 = high2.lowpassResponse(hz, sampleRate);
        const C lp4 = high4a.lowpassResponse(hz, sampleRate) * high4b.lowpassResponse(hz, sampleRate);
        const C lowCut = 1.0 + (hp2 + (hp4 - hp2) * steep - 1.0) * low;
        const C highCut = 1.0 + (lp2 + (lp4 - lp2) * steep - 1.0) * high;
        const C total = output * ((1.0 - mix) + mix * input * lowCut * highCut);
        return dsp::gainToDb(std::abs(total));
    }
};

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    graph_ = {kGraphPanel.x + 52, kGraphPanel.y + 44, kGraphPanel.w - 68, kGraphPanel.h - 76};

    // Controls are added in keyboard (Tab) order.
    ui().knob(kLowCut, {48, 432, 92, 100});
    ui().toggle(kLowCutOn, {156, 466, 112, 30}).withLabel("Low Cut");
    ui().segmented(kSlope, {kFilterPanel.right() - 200, kFilterPanel.y + 7, 184, 24});
    ui().toggle(kHighCutOn, {380, 466, 112, 30}).withLabel("High Cut");
    ui().knob(kHighCut, {508, 432, 92, 100});
    ui().knob(kInputGain, {654, 122, 118, 122});
    ui().knob(kMix, {654, 264, 118, 122});
    ui().knob(kOutputGain, {654, 406, 118, 122});
    ui().toggle(kBypass, {kEditorWidth - 24 - 112, 16, 112, 28});
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

// --- the spectrum ---------------------------------------------------------------------------

void Editor::onAudio(size_t) { fresh_ = true; }

void Editor::animate(double seconds) {
    // At most 30 analyses a second, and only while there is audio to see.
    sinceAnalysis_ += seconds;
    if (sinceAnalysis_ < 1.0 / 30.0) return;
    const bool changed = fresh_ || spectrum_.moving();
    const double rate = host().sampleRate() > 0.0 ? host().sampleRate() : 48000.0;
    if (fresh_) spectrum_.analyse(audio(), Signal::output(), rate, sinceAnalysis_);
    else spectrum_.fall(sinceAnalysis_);
    sinceAnalysis_ = 0.0;
    fresh_ = false;
    if (changed) markDirty();
}

// --- graph geometry -----------------------------------------------------------------------

float Editor::xOf(double hz) const {
    const double t = std::log(std::clamp(hz, kMinHz, kMaxHz) / kMinHz) / std::log(kMaxHz / kMinHz);
    return graph_.x + graph_.w * static_cast<float>(t);
}

double Editor::hzAt(float x) const {
    const double t = std::clamp((x - graph_.x) / graph_.w, 0.0f, 1.0f);
    return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float Editor::yOf(double db) const {
    const double t = (std::clamp(db, kBottomDb, kTopDb) - kTopDb) / (kBottomDb - kTopDb);
    return graph_.y + graph_.h * static_cast<float>(t);
}

Editor::Response Editor::response() const {
    Response r;
    r.sampleRate = host().sampleRate() > 0.0 ? host().sampleRate() : 48000.0;
    const double low = effective(kLowCut), high = effective(kHighCut);
    r.low2.setCoefficients(low, Processor::kQ2, r.sampleRate);
    r.low4a.setCoefficients(low, Processor::kQ4a, r.sampleRate);
    r.low4b.setCoefficients(low, Processor::kQ4b, r.sampleRate);
    r.high2.setCoefficients(high, Processor::kQ2, r.sampleRate);
    r.high4a.setCoefficients(high, Processor::kQ4a, r.sampleRate);
    r.high4b.setCoefficients(high, Processor::kQ4b, r.sampleRate);
    r.input = dsp::dbToGain(effective(kInputGain));
    r.output = dsp::dbToGain(effective(kOutputGain));
    r.mix = effective(kMix);
    r.low = effective(kLowCutOn) > 0.5 ? 1.0 : 0.0;
    r.high = effective(kHighCutOn) > 0.5 ? 1.0 : 0.0;
    r.steep = effective(kSlope) > 0.5 ? 1.0 : 0.0;
    return r;
}

double Editor::responseDb(double hz) const { return response().db(hz); }

namespace {
constexpr uint32_t kHandleParams[2] = {kLowCut, kHighCut};
constexpr uint32_t kHandleToggles[2] = {kLowCutOn, kHighCutOn};
}  // namespace

int Editor::handleAt(float x, float y) const {
    for (int i = 0; i < 2; ++i) {
        const double hz = plain(kHandleParams[i]);
        const float dx = x - xOf(hz), dy = y - yOf(responseDb(hz));
        if (dx * dx + dy * dy <= 12.0f * 12.0f) return i;
    }
    return -1;
}

// --- painting ----------------------------------------------------------------------------------

void Editor::paint(NVGcontext* vg) {
    const Theme& t = theme();
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, width(), height());
    nvgFillColor(vg, rgb(t.ground));
    nvgFill(vg);

    paintHeader(vg);
    w::panel(vg, t, kGraphPanel, "Response");
    paintResponse(vg);
    w::panel(vg, t, kFilterPanel, "Filter");
    w::panel(vg, t, kLevelPanel, "Level");
    paintMeters(vg);
    drawControls(vg);
}

void Editor::paintHeader(NVGcontext* vg) {
    const Theme& t = theme();
    nvgBeginPath(vg);
    nvgRect(vg, kHeader.x, kHeader.y, kHeader.w, kHeader.h);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, kHeader.h, rgb(t.surface), rgb(t.ground)));
    nvgFill(vg);
    w::hairline(vg, 0, kHeader.bottom() - 0.5f, kHeader.w, kHeader.bottom() - 0.5f, rgb(t.line));

    // The product name comes from product.cmake, like every other identity.
    char name[64];
    size_t n = 0;
    for (const char* s = info::kName; *s && n + 1 < sizeof(name); ++s)
        name[n++] = (*s >= 'a' && *s <= 'z') ? static_cast<char>(*s - 'a' + 'A') : *s;
    name[n] = '\0';
    nvgSave(vg);
    nvgFontFace(vg, font::kDisplay);
    nvgFontSize(vg, 25.0f);
    nvgTextLetterSpacing(vg, 2.5f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, rgb(t.text));
    nvgText(vg, 26, 27, name, nullptr);
    nvgRestore(vg);
    w::caption(vg, 27, 47, info::kVendor, rgb(t.textFaint), w::Align::Left, 9.0f);

    char build[96];
    std::snprintf(build, sizeof(build), "v%s  ·  latency %d", info::kVersion, host().latencySamples());
    w::label(vg, kEditorWidth - 24 - 112 - 18, 30, build, 10.5f, rgb(t.textFaint), w::Align::Right);
}

void Editor::paintResponse(NVGcontext* vg) {
    const Response filter = response();
    const Theme& t = theme();
    w::well(vg, t, graph_.inset(-1.0f), 4.0f);

    // Grid: decades emphasised, 0 dB brightest.
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const float x = std::floor(xOf(hz)) + 0.5f;
        const bool decade = hz == 100.0 || hz == 1000.0 || hz == 10000.0;
        w::hairline(vg, x, graph_.y, x, graph_.bottom(), rgba(decade ? t.lineLit : t.line, decade ? 0.8f : 0.6f));
        char text[16];
        if (hz >= 1000.0)
            std::snprintf(text, sizeof(text), "%dk", static_cast<int>(hz / 1000.0));
        else
            std::snprintf(text, sizeof(text), "%d", static_cast<int>(hz));
        w::number(vg, x, graph_.bottom() + 14.0f, text, 9.5f, rgb(t.textFaint), w::Align::Center, false);
    }
    for (int db = 12; db >= -36; db -= 12) {
        const float y = std::floor(yOf(db)) + 0.5f;
        w::hairline(vg, graph_.x, y, graph_.right(), y, db == 0 ? rgba(t.lineLit, 1.0f) : rgba(t.line, 0.7f));
        char text[16];
        std::snprintf(text, sizeof(text), "%+d", db);
        w::number(vg, graph_.x - 10.0f, y, db == 0 ? "0" : text, 9.5f, rgb(t.textFaint), w::Align::Right, false);
    }
    w::label(vg, graph_.x - 10.0f, graph_.bottom() + 14.0f, "Hz", 9.5f, rgb(t.textFaint), w::Align::Right);

    // What is playing, under the curve: the output's spectrum, its 0 dBFS
    // at the graph's +12 line, falling 60 dB to its floor.
    const Axis hz = Axis::logarithmic(kMinHz, kMaxHz, graph_.x, graph_.right());
    const Axis level = Axis::linear(-60.0, 0.0, graph_.bottom(), yOf(kTopDb));
    w::spectrum(vg, graph_, spectrum_, hz, level, rgba(t.accent2, 0.7f));

    // The curve, one point per two design units, clipped to the well.
    nvgSave(vg);
    nvgScissor(vg, graph_.x, graph_.y, graph_.w, graph_.h);
    const int points = static_cast<int>(graph_.w / 2.0f) + 1;
    auto trace = [&] {
        nvgBeginPath(vg);
        for (int i = 0; i < points; ++i) {
            const float x = graph_.x + graph_.w * static_cast<float>(i) / static_cast<float>(points - 1);
            const float y = yOf(filter.db(hzAt(x)));
            if (i == 0) nvgMoveTo(vg, x, y);
            else nvgLineTo(vg, x, y);
        }
    };
    // Fill between the curve and 0 dB: the shaded area is what the filter
    // section takes away (or adds), and a flat response shows none.
    const bool bypassed = plain(kBypass) > 0.5;
    const NVGcolor curve = bypassed ? rgb(t.textFaint) : rgb(t.accent);
    trace();
    nvgLineTo(vg, graph_.right(), yOf(0.0));
    nvgLineTo(vg, graph_.x, yOf(0.0));
    nvgClosePath(vg);
    nvgFillColor(vg, withAlpha(curve, 0.13f));
    nvgFill(vg);
    trace();
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, withAlpha(curve, 0.18f));
    nvgStrokeWidth(vg, 7.0f);
    nvgStroke(vg);
    nvgStrokeColor(vg, curve);
    nvgStrokeWidth(vg, 2.0f);
    nvgStroke(vg);
    nvgRestore(vg);

    // Draggable cutoff handles: filled when that filter is switched on.
    for (int i = 0; i < 2; ++i) {
        const double cutoff = plain(kHandleParams[i]);
        const bool on = plain(kHandleToggles[i]) > 0.5;
        const bool hot = i == hovering_ || i == dragging_;
        const float x = xOf(cutoff), y = yOf(filter.db(cutoff));
        w::hairline(vg, std::floor(x) + 0.5f, graph_.y, std::floor(x) + 0.5f, graph_.bottom(),
                    rgba(on ? t.accent : t.textFaint, hot ? 0.45f : 0.18f));
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, hot ? 7.5f : 6.0f);
        nvgFillColor(vg, on ? rgb(t.accent) : rgb(t.recess));
        nvgFill(vg);
        nvgStrokeColor(vg, on ? rgb(t.ground) : rgb(hot ? t.text : t.textMuted));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
        if (hot) {
            char text[32];
            const supple::ParamDef& p = *supple::params::find(kHandleParams[i]);
            supple::formatValue(p, host().value(kHandleParams[i]), text, sizeof(text));
            const float tx = std::clamp(x, graph_.x + 30.0f, graph_.right() - 30.0f);
            w::number(vg, tx, std::max(graph_.y + 10.0f, y - 18.0f), text, 10.5f, rgb(t.text), w::Align::Center);
        }
    }
}

void Editor::paintMeters(NVGcontext* vg) {
    const Theme& t = theme();
    const Rect in{786, 124, 34, 382}, out{832, 124, 34, 382};
    w::meter(vg, t, in, inputMeters(), meterChannels());
    w::meter(vg, t, out, outputMeters(), meterChannels());
    w::caption(vg, in.cx(), in.bottom() + 15.0f, "In", rgb(t.textMuted), w::Align::Center, 9.0f);
    w::caption(vg, out.cx(), out.bottom() + 15.0f, "Out", rgb(t.textMuted), w::Align::Center, 9.0f);
    const float barTop = in.y + 9.0f, barHeight = in.h - 9.0f;
    for (int db : {0, -6, -12, -24, -48}) {
        const float y = barTop + barHeight * (1.0f - w::meterPosition(static_cast<float>(db)));
        char text[16];
        std::snprintf(text, sizeof(text), "%d", db);
        w::number(vg, in.x - 6.0f, y, text, 8.5f, rgb(t.textFaint), w::Align::Right, false);
    }
}

// --- the graph's handles ------------------------------------------------------------------------

Cursor Editor::cursor() const {
    return dragging_ >= 0 || hovering_ >= 0 ? Cursor::LeftRight : EditorBase::cursor();
}

bool Editor::onPointerDown(float x, float y, int button, uint32_t, bool doubleClick) {
    if (button != 0) return false;
    const int h = handleAt(x, y);
    if (h < 0) return false;
    const int control = ui().find(kHandleParams[h]);
    if (doubleClick) {
        ui().dragEdit(control, supple::hostDefault(*supple::params::find(kHandleParams[h])));
        ui().endDragEdit();
        return true;
    }
    dragging_ = h;
    return true;
}

bool Editor::onPointerMove(float x, float y, uint32_t) {
    if (dragging_ >= 0) {
        const supple::ParamDef& p = *supple::params::find(kHandleParams[dragging_]);
        ui().dragEdit(ui().find(p.id), supple::toHost(p, hzAt(x)));
        return true;
    }
    const int h = handleAt(x, y);
    if (h != hovering_) {
        hovering_ = h;
        markDirty();
    }
    return false;  // let the controls track hover too
}

bool Editor::onPointerUp(float, float, int button, uint32_t) {
    if (button != 0 || dragging_ < 0) return false;
    ui().endDragEdit();
    dragging_ = -1;
    return true;
}

void Editor::onCancel() {
    if (dragging_ >= 0) ui().endDragEdit();
    dragging_ = -1;
    hovering_ = -1;
}

}  // namespace product
