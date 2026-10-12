// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <cstdio>

#include "gui/Layout.h"
#include "gui/Widgets.h"
#include "Params.h"
#include "ProductInfo.h"
#include "Properties.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;

namespace {
constexpr Rect kImpulsePanel{24, 76, 512, 112};
constexpr Rect kRoomPanel{24, 200, 420, 136};
constexpr Rect kOutputPanel{456, 200, 80, 136};
}  // namespace

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    const Rect body = panelBody(kImpulsePanel);
    ui().fileSlot(kImpulse, {body.x, body.y, body.w, 30}).label = "Drop a WAV, AIFF or FLAC here, or click";
    const Grid knobs = row(panelBody(kRoomPanel), 4, 8.0f);
    ui().knob(kMix, knobs[0]);
    ui().knob(kPredelay, knobs[1]);
    ui().knob(kTone, knobs[2]);
    ui().knob(kOutput, knobs[3]);
    ui().toggle(kBypass, {kEditorWidth - 24 - 96, 20, 96, 26});
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

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
    nvgText(vg, 26, 27, "ROOM", nullptr);
    nvgRestore(vg);
    char line[48];
    std::snprintf(line, sizeof(line), "%s  \xC2\xB7  v%s", info::kVendor, info::kVersion);
    w::caption(vg, 27, 47, line, rgb(t.textFaint), w::Align::Left, 9.0f);

    w::panel(vg, t, kImpulsePanel, "Impulse response");
    // What plays when no file does: the slot says what became of the file.
    const supple::FileStatus status = host().fileStatus(kImpulse);
    const bool own = status.state != supple::FileStatus::State::Ready;
    const Rect body = panelBody(kImpulsePanel);
    w::label(vg, body.x + 2, body.y + 47, own ? "Playing the built-in room" : "Playing the file", 10.5f,
             own ? rgb(t.textMuted) : rgb(t.accent));
    w::panel(vg, t, kRoomPanel, "Room");
    w::panel(vg, t, kOutputPanel, "Level");
    const Rect meter = panelBody(kOutputPanel).centred(30, panelBody(kOutputPanel).h);
    w::meter(vg, t, meter, outputMeters(), meterChannels());
    drawControls(vg);
}

}  // namespace product
