// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "Editor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Widgets.h"
#include "eq/processor.h"
#include "supple/analysis/analysis.h"
extern "C" {
extern const unsigned char eq_font_mono[];
extern const unsigned int eq_font_mono_size;
extern const unsigned char eq_font_mono_medium[];
extern const unsigned int eq_font_mono_medium_size;
extern const unsigned char eq_font_display[];
extern const unsigned int eq_font_display_size;
extern const unsigned char eq_font_ui[];
extern const unsigned int eq_font_ui_size;
extern const unsigned char eq_font_ui_semibold[];
extern const unsigned int eq_font_ui_semibold_size;
}

namespace eq::gui {
namespace se = supple::eq;
namespace pa = supple::params;
namespace {
constexpr float gx = 44, gy = 90, gw = 812, gh = 244;
float fx(double hz) { return gx + gw * static_cast<float>(std::log(hz / 20) / std::log(1000.)); }
float fy(double db, bool zoom) {
    return gy + gh * .5f - gh * static_cast<float>(db) / (zoom ? 24.f : 48.f);
}
void text(NVGcontext* vg, float x, float y, std::string_view str, float size, unsigned colour,
          int align = NVG_ALIGN_LEFT) {
    nvgFontFace(vg, kFontUi);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, align | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, rgb(colour));
    nvgText(vg, x, y, str.data(), str.data() + str.size());
}
se::Values values(EditorHost& h) {
    se::Values v{};
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = h.paramValue(static_cast<uint32_t>(i));
    return v;
}
bool choice(int id) { return kParams[static_cast<std::size_t>(id)].type != pa::Type::Float; }
}  // namespace
bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    const int a = nvgCreateFontMem(vg, kFontMono, const_cast<unsigned char*>(eq_font_mono),
                                   eq_font_mono_size, 0);
    const int b =
        nvgCreateFontMem(vg, kFontMonoMed, const_cast<unsigned char*>(eq_font_mono_medium),
                         eq_font_mono_medium_size, 0);
    const int c = nvgCreateFontMem(vg, kFontDisplay, const_cast<unsigned char*>(eq_font_display),
                                   eq_font_display_size, 0);
    const int d =
        nvgCreateFontMem(vg, kFontUi, const_cast<unsigned char*>(eq_font_ui), eq_font_ui_size, 0);
    const int e = nvgCreateFontMem(vg, kFontUiSemi, const_cast<unsigned char*>(eq_font_ui_semibold),
                                   eq_font_ui_semibold_size, 0);
    fontsReady_ = a >= 0 && b >= 0 && c >= 0 && d >= 0 && e >= 0;
    return fontsReady_;
}

void Editor::pump(double seconds) {
    if (scrollId_ >= 0) {
        scrollAge_ += static_cast<float>(seconds);
        if (scrollAge_ >= .25f) closeScroll();
    }
    Frame f;
    while (host_.telemetry().pop(f)) {
        std::copy(std::begin(f.input), std::end(f.input),
                  pre_.begin() + static_cast<std::ptrdiff_t>(spectrumFill_));
        std::copy(std::begin(f.output), std::end(f.output),
                  post_.begin() + static_cast<std::ptrdiff_t>(spectrumFill_));
        std::copy(std::begin(f.inputRight), std::end(f.inputRight),
                  preRight_.begin() + static_cast<std::ptrdiff_t>(spectrumFill_));
        std::copy(std::begin(f.outputRight), std::end(f.outputRight),
                  postRight_.begin() + static_cast<std::ptrdiff_t>(spectrumFill_));
        spectrumFill_ += 256;
        if (spectrumFill_ == pre_.size()) {
            supple::analysis::Options options;
            options.loudness = options.harmonic = options.envelope = options.stereo = false;
            for (int c = 0; c < 2; ++c) {
                const float* input = c ? post_.data() : pre_.data();
                const float* ptrs[] = {input, c ? postRight_.data() : preRight_.data()};
                const auto report = supple::analysis::analyze(
                    {ptrs, 4096, host_.sampleRate(), supple::analysis::Layout::Stereo}, options);
                if (report && report->spectral) {
                    auto& spectrum = c ? spectrumPost_ : spectrumPre_;
                    for (std::size_t i = 0; i < spectrum.size(); ++i)
                        spectrum[i] = .7 * spectrum[i] + .3 * report->spectral->ltasDb[i];
                }
            }
            spectrumFill_ = 0;
            dirty_ = true;
        }
    }
    for (const auto& p : kParams)
        if (shown_[p.id] != host_.paramValue(p.id)) dirty_ = true;
}
int Editor::controlAt(float x, float y) const {
    if (y >= 346 && y < 507 && x >= 20 && x < 880) {
        const int b = static_cast<int>((x - 20) / 107.5f), f = static_cast<int>((y - 346) / 23);
        if (b >= 0 && b < 8 && f < 7) {
            if (f == 5) {
                const int t = static_cast<int>(host_.paramValue(static_cast<uint32_t>(b * 7 + 1)));
                if (t != se::HighPass && t != se::LowPass) return -1;
            }
            return b * 7 + f;
        }
    }
    if (y >= 518 && y <= 548) {
        if (x >= 475 && x < 605) return static_cast<int>(se::kPhase);
        if (x >= 620 && x < 750) return static_cast<int>(se::kAutoGain);
        if (x >= 765 && x < 880) return static_cast<int>(se::kOutput);
    }
    return -1;
}
int Editor::handleAt(float x, float y) const {
    const auto v = values(host_);
    int best = -1;
    float dist = 225;
    for (std::size_t b = 0; b < 8; ++b) {
        const auto i = b * 7;
        const float dx = x - fx(v[i + se::Freq]), dy = y - fy(v[i + se::Gain], zoom_);
        if (dx * dx + dy * dy < dist) {
            dist = dx * dx + dy * dy;
            best = static_cast<int>(b);
        }
    }
    return best;
}
void Editor::setNormal(uint32_t id, double n) {
    host_.setParam(id, pa::fromNormalized(kParams[id], n));
    dirty_ = true;
}
void Editor::closeScroll() {
    if (scrollId_ >= 0) host_.endEdit(static_cast<uint32_t>(scrollId_));
    scrollId_ = -1;
}
bool Editor::onMouseDown(float x, float y) {
    onFocusLost();
    if (menuBand_ >= 0) {
        if (x >= 674 && x <= 852 && y >= 94 && y < 94 + 8 * 25) {
            const auto id = static_cast<uint32_t>(menuBand_ * 7 + 1);
            host_.beginEdit(id);
            host_.setParam(id, static_cast<int>((y - 94) / 25));
            host_.endEdit(id);
        }
        menuBand_ = -1;
        dirty_ = true;
        return true;
    }
    if (x > 810 && y > 45 && y < 72) {
        zoom_ = !zoom_;
        dirty_ = true;
        return true;
    }
    const int b = handleAt(x, y);
    if (b >= 0 && y >= gy && y <= gy + gh) {
        dragId_ = b * 7 + static_cast<int>(se::Freq);
        graphDrag_ = true;
        focus_ = dragId_;
        host_.beginEdit(static_cast<uint32_t>(dragId_));
        host_.beginEdit(static_cast<uint32_t>(b * 7 + se::Gain));
        return true;
    }
    const int id = controlAt(x, y);
    if (id < 0) return false;
    focus_ = id;
    dirty_ = true;
    if (choice(id)) {
        const auto& p = kParams[static_cast<std::size_t>(id)];
        host_.beginEdit(p.id);
        const auto next = host_.paramValue(p.id) + 1;
        host_.setParam(p.id, next > p.max ? p.min : next);
        host_.endEdit(p.id);
        return true;
    }
    dragId_ = id;
    graphDrag_ = false;
    dragY_ = y;
    dragNormal_ = pa::toNormalized(kParams[static_cast<std::size_t>(id)],
                                   host_.paramValue(static_cast<uint32_t>(id)));
    host_.beginEdit(static_cast<uint32_t>(id));
    return true;
}
void Editor::onPointerDown(float x, float y, int button, uint32_t) {
    if (button == 2) {
        onFocusLost();
        menuBand_ = handleAt(x, y);
        dirty_ = true;
    } else if (button == 0)
        onMouseDown(x, y);
}
void Editor::onMouseMove(float x, float y, bool fine) {
    if (dragId_ < 0) return;
    if (graphDrag_) {
        const auto id = static_cast<uint32_t>(dragId_);
        host_.setParam(
            id, std::clamp(20 * std::pow(1000., static_cast<double>((x - gx) / gw)), 10., 30000.));
        host_.setParam(
            id + 1, std::clamp(static_cast<double>((gy + gh * .5f - y) / gh) * (zoom_ ? 24. : 48.),
                               -30., 30.));
        dirty_ = true;
    } else
        setNormal(static_cast<uint32_t>(dragId_),
                  dragNormal_ + (dragY_ - y) * (fine ? .0005 : .005));
}
void Editor::onMouseUp() {
    if (dragId_ >= 0) {
        host_.endEdit(static_cast<uint32_t>(dragId_));
        if (graphDrag_) host_.endEdit(static_cast<uint32_t>(dragId_ + 1));
    }
    dragId_ = -1;
    graphDrag_ = false;
}
void Editor::onDoubleClick(float x, float y) {
    onFocusLost();
    const int b = handleAt(x, y);
    const int id = b >= 0 ? b * 7 : controlAt(x, y);
    if (id < 0) return;
    host_.beginEdit(static_cast<uint32_t>(id));
    host_.setParam(static_cast<uint32_t>(id), b >= 0
                                                  ? 1 - host_.paramValue(static_cast<uint32_t>(id))
                                                  : kParams[static_cast<std::size_t>(id)].def);
    host_.endEdit(static_cast<uint32_t>(id));
    dirty_ = true;
}
bool Editor::onScroll(float x, float y, float dy) {
    const int b = handleAt(x, y);
    const int id =
        (b >= 0 && y >= gy && y <= gy + gh) ? b * 7 + static_cast<int>(se::Q) : controlAt(x, y);
    if (id < 0) return false;
    onMouseUp();
    if (scrollId_ != id) {
        closeScroll();
        scrollId_ = id;
        host_.beginEdit(static_cast<uint32_t>(id));
    }
    const auto& p = kParams[static_cast<std::size_t>(id)];
    if (choice(id))
        host_.setParam(p.id, std::clamp(host_.paramValue(p.id) + (dy > 0 ? 1 : -1), p.min, p.max));
    else
        setNormal(p.id, pa::toNormalized(p, host_.paramValue(p.id)) + dy * .025);
    scrollAge_ = 0;
    dirty_ = true;
    return true;
}
void Editor::onFocusLost() {
    onMouseUp();
    closeScroll();
}
void Editor::onKey(EditorKey key, bool fine) {
    onFocusLost();
    if (key == EditorKey::Tab) {
        focus_ = (focus_ + (fine ? -1 : 1) + kNumParams) % kNumParams;
        dirty_ = true;
        return;
    }
    if (focus_ < 0) focus_ = 0;
    const auto& p = kParams[static_cast<std::size_t>(focus_)];
    double v = host_.paramValue(p.id);
    if (key == EditorKey::Home)
        v = p.min;
    else if (key == EditorKey::End)
        v = p.max;
    else if (choice(focus_)) {
        const bool down = key == EditorKey::Left || key == EditorKey::Down;
        v = std::clamp(v + (down ? -1 : 1), p.min, p.max);
    } else {
        const bool down = key == EditorKey::Left || key == EditorKey::Down;
        v = pa::fromNormalized(p, pa::toNormalized(p, v) + (down ? -1 : 1) * (fine ? .001 : .01));
    }
    host_.beginEdit(p.id);
    host_.setParam(p.id, v);
    host_.endEdit(p.id);
    dirty_ = true;
}
void Editor::render(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kDesignH);
    nvgFillColor(vg, rgb(col::ink));
    nvgFill(vg);
    text(vg, 22, 13, "Supple Audio", 9, col::dust);
    text(vg, 22, 37, supple::eq::kDisplayName, 26, col::bone);
    text(vg, 158, 36, "PARAMETRIC EQ", 11, col::dim);
    text(vg, 44, 59, "RESPONSE  /  INPUT + OUTPUT SPECTRUM", 10, col::dust);
    text(vg, 855, 59, zoom_ ? "±12 dB" : "±24 dB", 11, col::accent, NVG_ALIGN_RIGHT);
    fillRoundRect(vg, gx, gy, gw, gh, 4, rgb(col::well));
    for (double hz : {20., 50., 100., 200., 500., 1000., 2000., 5000., 10000., 20000.}) {
        const auto x = fx(hz);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x, gy);
        nvgLineTo(vg, x, gy + gh);
        nvgStrokeColor(vg, rgba(col::edge, .7f));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
        text(vg, x, gy + gh + 9, pa::formatCompact(kParams[2], hz), 9, col::dim, NVG_ALIGN_CENTER);
    }
    for (int db = -24; db <= 24; db += 6) {
        const auto y = fy(db, zoom_);
        if (y < gy || y > gy + gh) continue;
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx, y);
        nvgLineTo(vg, gx + gw, y);
        nvgStrokeColor(vg, rgba(col::edge, db == 0 ? 1.f : .5f));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
        text(vg, gx - 8, y, std::to_string(db), 9, col::dim, NVG_ALIGN_RIGHT);
    }
    nvgSave(vg);
    nvgScissor(vg, gx, gy, gw, gh);
    for (int c = 0; c < 2; ++c) {
        const auto& spectrum = c ? spectrumPost_ : spectrumPre_;
        nvgBeginPath(vg);
        for (std::size_t i = 0; i < spectrum.size(); ++i) {
            const float x = fx(supple::analysis::thirdOctaveCentre(static_cast<int>(i)));
            const float y =
                gy + gh - gh * static_cast<float>(std::clamp((spectrum[i] + 90) / 90, 0., 1.));
            if (i == 0)
                nvgMoveTo(vg, x, y);
            else
                nvgLineTo(vg, x, y);
        }
        nvgStrokeWidth(vg, 1);
        nvgStrokeColor(vg, rgba(c ? col::dust : col::accent, .25f));
        nvgStroke(vg);
    }
    auto v = values(host_);
    const double autoDb = v[se::kAutoGain] >= .5 ? se::compensationDb(v, host_.sampleRate()) : 0;
    auto curveValues = v;
    curveValues[se::kAutoGain] = 0;
    for (int curve = -1; curve < 8; ++curve) {
        if (curve >= 0 && v[static_cast<std::size_t>(curve) * 7] < .5) continue;
        nvgBeginPath(vg);
        for (int k = 0; k <= 220; ++k) {
            const double hz = 20 * std::pow(1000., k / 220.);
            const double db = curve < 0
                                  ? se::magnitudeDb(curveValues, hz, host_.sampleRate()) + autoDb
                                  : se::bandMagnitudeDb(v, static_cast<std::size_t>(curve), hz,
                                                        host_.sampleRate());
            const float x = fx(hz), y = fy(db, zoom_);
            if (k == 0)
                nvgMoveTo(vg, x, y);
            else
                nvgLineTo(vg, x, y);
        }
        nvgStrokeColor(vg, rgba(col::accent, curve < 0 ? 1.f : .24f));
        nvgStrokeWidth(vg, curve < 0 ? 2.f : 1.f);
        nvgStroke(vg);
    }
    for (std::size_t b = 0; b < 8; ++b) {
        const auto i = b * 7;
        const float x = fx(v[i + se::Freq]), y = fy(v[i + se::Gain], zoom_);
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, 10);
        nvgFillColor(vg, rgb(v[i] > .5 ? col::accent : col::edgeHi));
        nvgFill(vg);
        text(vg, x, y, std::to_string(b + 1), 10, v[i] > .5 ? col::ink : col::dust,
             NVG_ALIGN_CENTER);
    }
    nvgRestore(vg);
    for (std::size_t b = 0; b < 8; ++b) {
        const float x = 20 + static_cast<float>(b) * 107.5f;
        fillRoundRect(vg, x, 346, 102, 161, 3, rgb(col::panel));
        for (std::size_t f = 0; f < 7; ++f) {
            const auto i = b * 7 + f;
            if (f == se::Slope && static_cast<int>(v[b * 7 + se::Type]) != se::HighPass &&
                static_cast<int>(v[b * 7 + se::Type]) != se::LowPass)
                continue;
            const float y = 357.5f + static_cast<float>(f) * 23;
            std::string label = std::string(kParams[i].key.substr(kParams[i].key.find('.') + 1));
            if (f == 0) label = std::to_string(b + 1) + " " + (v[i] > .5 ? "ON" : "OFF");
            text(vg, x + 6, y, label, 9, col::dim);
            if (f != 0)
                text(vg, x + 96, y,
                     (kParams[i].type == pa::Type::Enum
                          ? std::string(kParams[i].choices[static_cast<std::size_t>(v[i])].label)
                          : pa::formatCompact(kParams[i], v[i])),
                     10, col::bone, NVG_ALIGN_RIGHT);
            if (focus_ == static_cast<int>(i))
                strokeRoundRect(vg, x + 1, y - 10, 100, 20, 2, rgb(col::accent), 1);
        }
    }
    text(vg, 20, 533, "Drag band: frequency / gain    Wheel: Q    Right-click: type", 10, col::dim);
    for (std::size_t i = se::kPhase; i < se::kParamCount; ++i) {
        const float x = i == se::kPhase ? 475.f : (i == se::kAutoGain ? 620.f : 765.f);
        text(vg, x, 531, std::string(kParams[i].name) + "  " + pa::format(kParams[i], v[i]), 11,
             col::bone);
        if (focus_ == static_cast<int>(i))
            strokeRoundRect(vg, x - 3, 518, 115, 27, 2, rgb(col::accent), 1);
    }
    if (menuBand_ >= 0) {
        fillRoundRect(vg, 674, 94, 178, 200, 4, rgb(col::panel));
        for (std::size_t i = 0; i < kParams[1].choices.size(); ++i)
            text(vg, 685, 106.5f + static_cast<float>(i) * 25, kParams[1].choices[i].label, 12,
                 col::bone);
    }
    for (std::size_t i = 0; i < v.size(); ++i)
        shown_[i] = host_.paramValue(static_cast<uint32_t>(i));
    dirty_ = false;
}
}  // namespace eq::gui
