// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Dsp.h"
#include "gui/Widgets.h"
#include "Params.h"
#include "Processor.h"
#include "ProductInfo.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;

namespace {
constexpr Rect kKeyPanel{24, 76, 200, 280};
constexpr Rect kDuckPanel{240, 76, 264, 280};
constexpr Rect kOutputPanel{520, 76, 176, 280};
constexpr Rect kHistoryPanel{24, 372, 672, 112};
constexpr double kHistorySeconds = 2.5;
constexpr float kFloorDb = -60.0f;
}  // namespace

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    ui().knob(kThreshold, {kKeyPanel.x + 8, kKeyPanel.y + 48, 104, 112});
    ui().knob(kDepth, {kDuckPanel.x + 76, kDuckPanel.y + 40, 112, 120});
    ui().knob(kAttack, {kDuckPanel.x + 24, kDuckPanel.y + 168, 96, 102});
    ui().knob(kRelease, {kDuckPanel.x + 144, kDuckPanel.y + 168, 96, 102});
    // An output parameter: the processor reports it, the readout shows it.
    ui().readout(kReduction, {kOutputPanel.x + 16, kOutputPanel.y + 222, kOutputPanel.w - 32, 34});
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

void Editor::onTelemetry(const supple::TelemetryFrame& frame) {
    reduction_.push(static_cast<float>(supple::dsp::gainToDb(frame.product.gain, -60.0)));
    const float db = static_cast<float>(supple::dsp::gainToDb(frame.product.key, -120.0));
    if (std::fabs(db - keyDb_) > 0.25f && (db > kFloorDb || keyDb_ > kFloorDb)) {
        keyDb_ = db;
        markDirty();  // only when what the panel shows changed
    }
}

// The sidechain's level against the threshold: above the line, it ducks.
void Editor::drawKey(NVGcontext* vg, Rect r) {
    const Theme& t = theme();
    w::well(vg, t, r, 4.0f);
    const Rect inner = r.inset(5.0f, 5.0f);
    auto yOf = [&](float db) { return inner.bottom() - inner.h * std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f); };
    const float threshold = static_cast<float>(effective(kThreshold));
    if (keyDb_ > kFloorDb) {
        nvgBeginPath(vg);
        nvgRect(vg, inner.x, yOf(keyDb_), inner.w, inner.bottom() - yOf(keyDb_));
        nvgFillColor(vg, keyDb_ > threshold ? rgb(t.accent) : rgba(t.accent2, 0.6f));
        nvgFill(vg);
    }
    w::hairline(vg, r.x - 4, yOf(threshold), r.right() + 4, yOf(threshold), rgb(t.text), 1.5f);
    w::caption(vg, r.x + r.w * 0.5f, r.bottom() + 14, "KEY", rgb(t.textFaint), w::Align::Center, 9.0f);
}

void Editor::paint(NVGcontext* vg) {
    const Theme& t = theme();
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, width(), height());
    nvgFillColor(vg, rgb(t.ground));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, width(), 60);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, 60, rgb(t.surface), rgb(t.ground)));
    nvgFill(vg);
    w::hairline(vg, 0, 59.5f, width(), 59.5f, rgb(t.line));
    nvgSave(vg);
    nvgFontFace(vg, font::kDisplay);
    nvgFontSize(vg, 25.0f);
    nvgTextLetterSpacing(vg, 2.5f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, rgb(t.text));
    char name[64];
    size_t n = 0;
    for (const char* s = info::kName; *s && n + 1 < sizeof(name); ++s)
        name[n++] = (*s >= 'a' && *s <= 'z') ? static_cast<char>(*s - 'a' + 'A') : *s;
    name[n] = '\0';
    nvgText(vg, 26, 27, name, nullptr);
    nvgRestore(vg);
    w::caption(vg, 27, 47, info::kVendor, rgb(t.textFaint), w::Align::Left, 9.0f);
    char version[32];
    std::snprintf(version, sizeof(version), "v%s", info::kVersion);
    // The track it sits on, as the host tells it: a ducker's key comes from
    // another track, so which one this is matters. Hosts that say nothing
    // leave the version alone there.
    const TrackInfo track = this->track();
    if (!track.name.empty() || track.hasColour) {
        const char* name = track.name.empty() ? "Track" : track.name.c_str();
        const float right = width() - 24, nameWidth = w::textWidth(vg, name, 12.0f, font::kUi);
        w::label(vg, right, 26, name, 12.0f, rgb(t.text), w::Align::Right);
        if (track.hasColour) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, right - nameWidth - 18, 20, 10, 12, 2.5f);
            nvgFillColor(vg, nvgRGBA(track.red, track.green, track.blue, 255));
            nvgFill(vg);
        }
        w::label(vg, right, 45, version, 10.0f, rgb(t.textFaint), w::Align::Right);
    } else {
        w::label(vg, width() - 24, 30, version, 10.5f, rgb(t.textFaint), w::Align::Right);
    }

    w::panel(vg, t, kKeyPanel, "Key");
    drawKey(vg, {kKeyPanel.right() - 60, kKeyPanel.y + 52, 28, 196});
    w::panel(vg, t, kDuckPanel, "Duck");
    w::panel(vg, t, kOutputPanel, "Output");
    w::meter(vg, t, {kOutputPanel.x + 24, kOutputPanel.y + 48, 30, 160}, outputMeters(), meterChannels(),
             outputRms());
    w::reductionMeter(vg, t, {kOutputPanel.x + 62, kOutputPanel.y + 57, 10, 151}, static_cast<float>(plain(kReduction)));

    // The last seconds: what came in (faint), what went out, and the
    // reduction between them, from the audio feed and each telemetry frame.
    w::panel(vg, t, kHistoryPanel, "History");
    const Rect graph = panelBody(kHistoryPanel, true, 16.0f);
    w::well(vg, t, graph.inset(-2.0f), 4.0f);
    const double rate = host().sampleRate() > 0.0 ? host().sampleRate() : 48000.0;
    const double span = std::min(kHistorySeconds, static_cast<double>(AudioHistory::kFrames - 1) / rate);
    w::waveform(vg, graph, audio(), Signal::input(), span, rate, rgba(t.textMuted, 0.35f));
    w::waveform(vg, graph, audio(), Signal::output(), span, rate, rgba(t.accent2, 0.8f));
    const Axis reductionDb = Axis::linear(-24.0, 0.0, graph.bottom(), graph.y);
    const double oldest = std::min(span * Processor::kTelemetryHz, static_cast<double>(reduction_.size() - 1));
    nvgSave(vg);
    nvgScissor(vg, graph.x, graph.y, graph.w, graph.h);
    nvgBeginPath(vg);
    for (size_t k = 0; k <= static_cast<size_t>(std::ceil(oldest)); ++k) {
        const double age = std::max(0.0, oldest - static_cast<double>(k));
        const size_t frame = static_cast<size_t>(age);
        const float value = reduction_.at(frame) + (reduction_.at(frame + 1) - reduction_.at(frame)) *
            static_cast<float>(age - static_cast<double>(frame));
        const float x = graph.right() - graph.w * static_cast<float>(age / oldest);
        if (k == 0) nvgMoveTo(vg, x, reductionDb.clamped(value));
        else nvgLineTo(vg, x, reductionDb.clamped(value));
    }
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, rgb(t.accent));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
    nvgRestore(vg);
    w::caption(vg, graph.right(), kHistoryPanel.y + 18, "IN   OUT   REDUCTION", rgb(t.textFaint), w::Align::Right, 8.5f);
    w::caption(vg, kOutputPanel.x + 124, kOutputPanel.y + 64, "MAIN + DUCKED", rgb(t.textFaint), w::Align::Center, 8.5f);
    w::caption(vg, kOutputPanel.x + 124, kOutputPanel.y + 80, "= INPUT", rgb(t.textFaint), w::Align::Center, 8.5f);
    drawControls(vg);
}

}  // namespace product
