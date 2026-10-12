// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <cmath>
#include <cstdio>

#include "gui/Widgets.h"
#include "Params.h"
#include "Processor.h"
#include "ProductInfo.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;

namespace {
constexpr Rect kTabs{24, 72, 220, 30};
constexpr Rect kRatePanel{24, 118, 208, 278};
constexpr Rect kShapePanel{248, 118, 264, 278};
constexpr Rect kOutputPanel{528, 118, 168, 278};
constexpr Rect kPresetPanel{24, 118, 672, 278};
constexpr int kIconButton = 1;  // a button drawn as an icon (the theme switch)
}  // namespace

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    ui().presetBar({kEditorWidth - 24 - 300, 16, 300, 28});
    // Light or dark: the product keeps both palettes, the editor swaps them.
    ui().button({kEditorWidth - 24 - 300 - 40, 16, 28, 28}, "Light", [this] {
            light_ = !light_;
            setTheme(light_ ? kLightTheme : kTheme);
        })
        .withVariant(kIconButton)
        .withTooltip("Switch between the light and the dark look.");

    tabs_ = ui().count();
    ui().tabs(kTabs, {"Pulse", "Presets"});

    ui().onPage(0);
    Rect rate = panelBody(kRatePanel);
    ui().segmented(kSync, rate.cutTop(28, 24));
    const Grid knobs = row(rate.cutTop(102), 2, 8);
    // The rate control not in use is shown, dimmed, and left alone.
    ui().knob(kRate, knobs.cell(0, 0)).enabledWhen([this] { return plain(kSync) < 0.5; });
    ui().knob(kDivision, knobs.cell(1, 0)).enabledWhen([this] { return plain(kSync) > 0.5; });

    Rect shape = panelBody(kShapePanel);
    ui().segmented(kShape, shape.cutTop(28, 76));  // the drawn shape goes in the gap
    // Across: the stereo phase; up: the depth. One drag moves both.
    ui().xyPad(kPhase, kDepth, shape);

    Rect output = panelBody(kOutputPanel);
    ui().knob(kOutput, output.cutTop(110).cutLeft(100));
    // An output parameter: the processor reports it, the readout shows it.
    ui().readout(kGain, output.cutBottom(34));

    ui().onPage(1);
    // Every preset, by category: choosing one loads it. Files dropped on
    // the editor load too, as does Import in the preset bar's menu.
    ui().presetList(panelBody(kPresetPanel).cutLeft(300));
    ui().onAllPages();
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

void Editor::drawControl(NVGcontext* vg, const Control& c, const ControlState& s) {
    if (c.variant != kIconButton) return EditorBase::drawControl(vg, c, s);
    const Theme& t = theme();
    if (s.hover || s.focused) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, c.bounds.x, c.bounds.y, c.bounds.w, c.bounds.h, 6.0f);
        nvgFillColor(vg, rgba(t.text, 0.08f));
        nvgFill(vg);
    }
    // The icon of the look it switches to, in the text colour.
    const NVGcolor colour = s.hover ? rgb(t.text) : rgb(t.textMuted);
    drawSvg(vg, light_ ? "moon.svg" : "sun.svg", c.bounds.inset(6.0f), &colour);
}

// One cycle of the oscillator at the current depth, as the left channel
// hears it, with the right channel's offset where Stereo Phase puts it.
void Editor::drawShape(NVGcontext* vg, Rect r) {
    const Theme& t = theme();
    w::well(vg, t, r, 5.0f);
    const Rect inner = r.inset(10.0f, 8.0f);
    const int shape = static_cast<int>(plain(kShape));
    const double depth = effective(kDepth), offset = plain(kPhase) / 360.0;
    auto yOf = [&](double gain) { return inner.bottom() - inner.h * static_cast<float>(gain); };
    for (int channel = 1; channel >= 0; --channel) {
        if (channel == 1 && offset == 0.0) continue;
        nvgBeginPath(vg);
        for (int k = 0; k <= 96; ++k) {
            const double phase = k / 96.0 + (channel ? offset : 0.0);
            const double g = Processor::tremolo(depth, Processor::shapeAt(shape, phase - std::floor(phase)));
            const float x = inner.x + inner.w * static_cast<float>(k) / 96.0f;
            if (k == 0) nvgMoveTo(vg, x, yOf(g));
            else nvgLineTo(vg, x, yOf(g));
        }
        nvgStrokeColor(vg, channel ? rgba(t.accent2, 0.55f) : rgb(t.accent));
        nvgStrokeWidth(vg, channel ? 1.5f : 2.0f);
        nvgStroke(vg);
    }
    w::label(vg, r.right() - 22, r.y + 10, "L", 9.5f, rgb(t.accent), w::Align::Left, true);
    if (offset != 0.0) w::label(vg, r.right() - 12, r.y + 10, "R", 9.5f, rgb(t.accent2), w::Align::Left, true);
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
    // The mark is an SVG among the product's assets, drawn as vectors.
    drawSvg(vg, "logo.svg", {24, 14, 32, 32});
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
    nvgText(vg, 66, 25, name, nullptr);
    nvgRestore(vg);
    char vendor[64];
    std::snprintf(vendor, sizeof(vendor), "%s  \xC2\xB7  v%s", info::kVendor, info::kVersion);
    w::caption(vg, 67, 45, vendor, rgb(t.textFaint), w::Align::Left, 9.0f);

    if (ui().page(tabs_) == 0) {
        w::panel(vg, t, kRatePanel, "Rate");
        w::panel(vg, t, kShapePanel, "Shape");
        Rect shape = panelBody(kShapePanel);
        shape.cutTop(28, 10);
        drawShape(vg, shape.cutTop(56));
        w::panel(vg, t, kOutputPanel, "Output");
        Rect output = panelBody(kOutputPanel);
        w::meter(vg, t, output.cutTop(150).cutRight(30), outputMeters(), meterChannels());
    } else {
        w::panel(vg, t, kPresetPanel, "Presets");
        Rect about = panelBody(kPresetPanel);
        about.cutLeft(300, 24);
        const char* lines[] = {"Choose a preset to load it.", "Save, rename and share them from the bar above.",
                               "Drop a .preset file anywhere on Pulse to load it."};
        for (int k = 0; k < 3; ++k)
            w::label(vg, about.x, about.y + 10 + 22.0f * static_cast<float>(k), lines[k], 11.5f,
                     k == 0 ? rgb(t.text) : rgb(t.textMuted));
    }
    drawControls(vg);
}

}  // namespace product
