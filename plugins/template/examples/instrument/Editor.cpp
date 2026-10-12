// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <cstdio>
#include <cstring>

#include "gui/Widgets.h"
#include "Params.h"
#include "ProductInfo.h"
#include "Theme.h"

namespace product {

using namespace supple::gui;
namespace w = supple::gui::widgets;

namespace {
constexpr Rect kVoicePanel{24, 76, 448, 150};
constexpr Rect kShapePanel{24, 242, 448, 150};
constexpr Rect kLevelPanel{488, 76, 208, 316};
constexpr Rect kKeysPanel{24, 408, 672, 86};

const char* noteName(int key, char* out, size_t size) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(out, size, "%s%d", names[((key % 12) + 12) % 12], key / 12 - 1);
    return out;
}
}  // namespace

Editor::Editor(EditorHost& host) : EditorBase(host, kTheme, kEditorWidth, kEditorHeight) {
    for (auto& k : keys_) k = -1;
    // The Sound panel: the waveform's switch on the left, the envelope's
    // four stages dragged on its curve in the rest.
    Rect sound = panelBody(kShapePanel);
    ui().segmented(kWaveform, sound.cutLeft(170.0f, 16.0f).centred(170.0f, 28.0f));
    ui().envelope(sound, kAttack, kDecay, kSustain, kRelease);
    ui().knob(kLevel, {kLevelPanel.x + 16, kLevelPanel.y + 44, 110, 120});
    ui().knob(kBendRange, {kLevelPanel.x + 24, kLevelPanel.y + 172, 94, 100});
    // Played with the pointer; lit by whatever is held, from anywhere.
    ui().keyboard(kKeysPanel.inset(16.0f, 12.0f), 36, 84);
    // Under the lamps: how the voices are used, and the glide between notes.
    Rect modes = panelBody(kVoicePanel);
    modes.cutTop(52.0f, 8.0f);
    const Grid two = row(modes, 2, 16.0f);
    ui().segmented(kMode, two[0].centred(two[0].w, 26.0f));
    ui().hslider(kGlide, two[1].centred(two[1].w, 30.0f));
}

std::unique_ptr<EditorBase> createEditor(EditorHost& host) { return std::make_unique<Editor>(host); }

void Editor::onTelemetry(const supple::TelemetryFrame& frame) {
    if (std::memcmp(keys_, frame.product.keys, sizeof(keys_)) != 0) {
        std::memcpy(keys_, frame.product.keys, sizeof(keys_));
        markDirty();  // only when what the panel shows changed
    }
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
    w::label(vg, width() - 24, 30, version, 10.5f, rgb(t.textFaint), w::Align::Right);

    // One lamp per voice, lit with the note it is playing.
    w::panel(vg, t, kVoicePanel, "Voices");
    const float cell = (kVoicePanel.w - 32) / Processor::kVoices;
    for (int v = 0; v < Processor::kVoices; ++v) {
        const Rect r{kVoicePanel.x + 16 + cell * v + 4, kVoicePanel.y + 44, cell - 8, 52};
        const bool on = keys_[v] >= 0;
        w::well(vg, t, r, 5.0f);
        if (on) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, r.x + 3, r.y + 3, r.w - 6, r.h - 6, 4);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, r.y, 0, r.bottom(), rgba(t.accent, 0.45f), rgba(t.accent, 0.12f)));
            nvgFill(vg);
            char text[8];
            w::number(vg, r.cx(), r.cy(), noteName(keys_[v], text, sizeof(text)), 13.0f, rgb(t.text), w::Align::Center);
        } else {
            w::number(vg, r.cx(), r.cy(), "-", 13.0f, rgb(t.textFaint), w::Align::Center, false);
        }
    }

    w::panel(vg, t, kShapePanel, "Sound");
    w::panel(vg, t, kLevelPanel, "Output");
    w::meter(vg, t, {kLevelPanel.right() - 58, kLevelPanel.y + 48, 38, 236}, outputMeters(), meterChannels());
    w::panel(vg, t, kKeysPanel);
    drawControls(vg);
}

}  // namespace product
