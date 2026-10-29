// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/Files.h"

namespace supple::gui::widgets {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kKnobStart = kPi * 0.75f;  // 7:30
constexpr float kKnobSweep = kPi * 1.5f;   // to 4:30

int nvgAlign(Align a) {
    return (a == Align::Center ? NVG_ALIGN_CENTER : a == Align::Right ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) |
           NVG_ALIGN_MIDDLE;
}

bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// "-3.0" -> "−3.0": the true minus sign sits on the figures' centre line.
// "-inf" -> "−∞".
void prettyNumber(const char* in, char* out, size_t size) {
    size_t o = 0;
    for (const char* s = in; *s && o + 4 < size; ++s) {
        if (*s == '-') {
            out[o++] = '\xE2';
            out[o++] = '\x88';
            out[o++] = '\x92';
        } else if (!std::strncmp(s, "inf", 3) && (s == in || !isLetter(s[-1])) && !isLetter(s[3])) {
            out[o++] = '\xE2';
            out[o++] = '\x88';
            out[o++] = '\x9E';
            s += 2;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
}

NVGcolor lerp(NVGcolor a, NVGcolor b, float t) { return nvgLerpRGBA(a, b, t); }

void roundedRect(NVGcontext* vg, Rect r, float radius) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
}

void focusOutline(NVGcontext* vg, const Theme& t, Rect r, float radius) {
    roundedRect(vg, r.inset(-3.0f), radius + 3.0f);
    nvgStrokeColor(vg, rgba(t.accent, 0.85f));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
}

void shadow(NVGcontext* vg, Rect r, float radius, float blur, float alpha) {
    nvgBeginPath(vg);
    nvgRect(vg, r.x - blur, r.y - blur * 0.5f, r.w + blur * 2.0f, r.h + blur * 2.0f);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, nvgBoxGradient(vg, r.x, r.y + blur * 0.25f, r.w, r.h, radius, blur,
                                    nvgRGBAf(0, 0, 0, alpha), nvgRGBAf(0, 0, 0, 0)));
    nvgFill(vg);
}

void arc(NVGcontext* vg, float cx, float cy, float r, float a0, float a1, NVGcolor c, float width) {
    if (std::fabs(a1 - a0) < 1e-4f) return;
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r, std::min(a0, a1), std::max(a0, a1), NVG_CW);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
}

// The text a control shows for its current value: the host's, with the
// editor's typography (numbers get theirs from number()).
ValueText valueText(const ControlState& s) {
    ValueText v = s.def ? describe(*s.def, s.value) : ValueText{};
    if (!std::strcmp(v.unit, "deg")) std::snprintf(v.unit, sizeof(v.unit), "\xC2\xB0");
    return v;
}

// Number and unit as one string: "-6.0 dB", "4.0:1", "90°".
void joined(const ValueText& v, char* out, size_t size) {
    const bool attached = v.unit[0] == ':' || !std::strcmp(v.unit, "\xC2\xB0");
    std::snprintf(out, size, "%s%s%s", v.number, v.unit[0] && !attached ? " " : "", v.unit);
}

float fitSize(NVGcontext* vg, const char* text, float size, float minSize, float width, const char* face) {
    const float w = textWidth(vg, text, size, face);
    return w > width && w > 0.0f ? std::max(minSize, size * width / w) : size;
}

// Words in `width` by the rule (fitText), drawn as label() does.
void fittedLabel(NVGcontext* vg, float x, float y, const char* text, float size, float minSize, float width,
                 NVGcolor colour, Align align, bool bold) {
    if (!text || !*text) return;
    std::string fitted(text);
    const float at = fitText(vg, fitted, size, minSize, width, bold ? font::kUiBold : font::kUi);
    label(vg, x, y, fitted.c_str(), at, colour, align, bold);
}

// A number likewise, as number() draws it; how wide it came out.
float fittedNumber(NVGcontext* vg, float x, float y, const char* text, float size, float minSize, float width,
                   NVGcolor colour, Align align, bool medium = true) {
    if (!text || !*text) return 0.0f;
    std::string fitted(text);
    const char* face = medium ? font::kMonoMedium : font::kMono;
    const float at = fitText(vg, fitted, size, minSize, width, face);
    number(vg, x, y, fitted.c_str(), at, colour, align, medium);
    return textWidth(vg, fitted.c_str(), at, face);
}

const char* optionLabel(const ParamDef& p, int index, char* scratch, size_t size) {
    if (p.labels && index >= 0 && index < p.labelCount) return p.labels[index];
    std::snprintf(scratch, size, "%d", static_cast<int>(p.hostMin()) + index);
    return scratch;
}

// The text field's face: a value in Plex Mono, text in Inter.
void textEntryFont(NVGcontext* vg, bool ascii) {
    nvgFontFace(vg, ascii ? font::kMonoMedium : font::kUi);
    nvgFontSize(vg, ascii ? 12.0f : 11.5f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
}

}  // namespace

// --- text ----------------------------------------------------------------------

void label(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour, Align align,
           bool bold) {
    if (!text || !*text) return;
    nvgSave(vg);
    nvgFontFace(vg, bold ? font::kUiBold : font::kUi);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, nvgAlign(align));
    nvgFillColor(vg, colour);
    nvgText(vg, x, y, text, nullptr);
    nvgRestore(vg);
}

void number(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour, Align align,
            bool medium) {
    if (!text || !*text) return;
    char pretty[64];
    prettyNumber(text, pretty, sizeof(pretty));
    nvgSave(vg);
    nvgFontFace(vg, medium ? font::kMonoMedium : font::kMono);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, nvgAlign(align));
    nvgFillColor(vg, colour);
    nvgText(vg, x, y, pretty, nullptr);
    nvgRestore(vg);
}

void caption(NVGcontext* vg, float x, float y, const char* text, NVGcolor colour, Align align, float size) {
    if (!text || !*text) return;
    char upper[96];
    size_t n = 0;
    for (const char* s = text; *s && n + 1 < sizeof(upper); ++s)
        upper[n++] = (*s >= 'a' && *s <= 'z') ? static_cast<char>(*s - 'a' + 'A') : *s;
    upper[n] = '\0';
    nvgSave(vg);
    nvgFontFace(vg, font::kUiBold);
    nvgFontSize(vg, size);
    nvgTextLetterSpacing(vg, size * 0.09f);
    nvgTextAlign(vg, nvgAlign(align));
    nvgFillColor(vg, colour);
    nvgText(vg, x, y, upper, nullptr);
    nvgRestore(vg);
}

float textWidth(NVGcontext* vg, const char* text, float size, const char* face) {
    if (!text || !*text) return 0.0f;
    nvgSave(vg);
    nvgFontFace(vg, face);
    nvgFontSize(vg, size);
    const float w = nvgTextBounds(vg, 0, 0, text, nullptr, nullptr);
    nvgRestore(vg);
    return w;
}

float fitText(NVGcontext* vg, std::string& text, float size, float minSize, float width, const char* face) {
    if (text.empty()) return size;
    width = std::max(0.0f, width);
    size = fitSize(vg, text.c_str(), size, std::min(minSize, size), width, face);
    nvgSave(vg);
    nvgFontFace(vg, face);
    nvgFontSize(vg, size);
    const auto widthOf = [&](const std::string& t) {
        return t.empty() ? 0.0f : nvgTextBounds(vg, 0, 0, t.data(), t.data() + t.size(), nullptr);
    };
    // NanoVG sizes a font in tenths, so a shrunken fit can read a hair over.
    const auto fits = [&](const std::string& t) { return widthOf(t) <= width + 0.5f; };
    if (!fits(text)) {
        // The longest run of whole characters that fits with an ellipsis
        // after it; a longer run is never narrower.
        std::vector<size_t> cuts;
        for (size_t i = 1; i < text.size(); ++i)
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
        const auto cut = [&](size_t k) {
            std::string t = text.substr(0, k > 0 ? cuts[k - 1] : 0);
            while (!t.empty() && t.back() == ' ') t.pop_back();
            return t + "\xE2\x80\xA6";
        };
        size_t lo = 0, hi = cuts.size();
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (fits(cut(mid))) lo = mid;
            else hi = mid - 1;
        }
        text = cut(lo);
        if (!fits(text)) text.clear();  // not even the ellipsis
    }
    nvgRestore(vg);
    return size;
}

float textEntryWidth(NVGcontext* vg, std::string_view text, bool ascii) {
    if (text.empty()) return 0.0f;
    nvgSave(vg);
    textEntryFont(vg, ascii);
    const float w = nvgTextBounds(vg, 0, 0, text.data(), text.data() + text.size(), nullptr);
    nvgRestore(vg);
    return w;
}

// --- furniture -------------------------------------------------------------------

void hairline(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor colour, float width) {
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x0, y0);
    nvgLineTo(vg, x1, y1);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
    nvgRestore(vg);
}

void panel(NVGcontext* vg, const Theme& t, Rect r, const char* title) {
    nvgSave(vg);
    shadow(vg, r, t.radius, 16.0f, 0.55f);
    roundedRect(vg, r, t.radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, r.y, r.x, r.bottom(), rgb(t.surface),
                                       lerp(rgb(t.surface), rgb(t.ground), 0.45f)));
    nvgFill(vg);
    roundedRect(vg, r.inset(0.5f), t.radius - 0.5f);
    nvgStrokeColor(vg, rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    hairline(vg, r.x + t.radius, r.y + 1.5f, r.right() - t.radius, r.y + 1.5f, rgba(t.lineLit, 0.55f));
    if (title) {
        caption(vg, r.x + 16.0f, r.y + 18.0f, title, rgb(t.textMuted));
        hairline(vg, r.x + 16.0f, r.y + 32.5f, r.right() - 16.0f, r.y + 32.5f, rgba(t.line, 0.9f));
    }
    nvgRestore(vg);
}

void well(NVGcontext* vg, const Theme& t, Rect r, float radius) {
    nvgSave(vg);
    roundedRect(vg, r, radius);
    nvgFillColor(vg, rgb(t.recess));
    nvgFill(vg);
    // Inner shadow along the top edge reads as depth.
    roundedRect(vg, r, radius);
    nvgFillPaint(vg, nvgBoxGradient(vg, r.x, r.y + 2.0f, r.w, r.h, radius, 6.0f, nvgRGBAf(0, 0, 0, 0),
                                    nvgRGBAf(0, 0, 0, 0.35f)));
    nvgFill(vg);
    roundedRect(vg, r.inset(0.5f), radius);
    nvgStrokeColor(vg, rgba(t.line, 0.9f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    nvgRestore(vg);
}

// --- controls --------------------------------------------------------------------

void control(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    if (!c.visible) return;
    switch (c.widget) {
        case Widget::TextField: return textField(vg, t, c, s);
        case Widget::File: return fileSlot(vg, t, c, s);
        case Widget::PresetBar: return presetBar(vg, t, c, s);
        case Widget::Button: return button(vg, t, c, s);
        case Widget::Tabs: return tabs(vg, t, c, s);
        case Widget::List: return list(vg, t, c, s);
        case Widget::Keyboard: return keyboard(vg, t, c, s);
        case Widget::Envelope: return envelope(vg, t, c, s);
        default: break;
    }
    if (!s.def) return;
    switch (c.widget) {
        case Widget::Momentary: button(vg, t, c, s); break;
        case Widget::XYPad: xyPad(vg, t, c, s); break;
        case Widget::Knob: knob(vg, t, c, s); break;
        case Widget::HSlider: hslider(vg, t, c, s); break;
        case Widget::VSlider: vslider(vg, t, c, s); break;
        case Widget::Toggle: toggle(vg, t, c, s); break;
        case Widget::Segmented: segmented(vg, t, c, s); break;
        case Widget::Dropdown: dropdown(vg, t, c, s); break;
        case Widget::Readout: readout(vg, t, c, s); break;
        default: break;
    }
}

void textField(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const float radius = std::min(5.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, b, radius);
    if (s.hover && !s.editing) {
        roundedRect(vg, b.inset(0.5f), radius);
        nvgStrokeColor(vg, rgb(t.lineLit));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
    }
    if (!s.editing) {
        const std::string text(s.text);
        const float width = b.w - 18.0f;
        if (!text.empty())
            fittedLabel(vg, b.x + 9.0f, b.cy() + 0.5f, text.c_str(), 11.5f, 11.5f, width, rgb(t.text), Align::Left, false);
        else if (c.label)
            fittedLabel(vg, b.x + 9.0f, b.cy() + 0.5f, c.label, 11.5f, 11.5f, width, rgb(t.textFaint), Align::Left, false);
    }
    if (s.focused && !s.editing) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void fileSlot(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    using State = FileStatus::State;
    const Rect b = c.bounds;
    const float radius = std::min(5.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, b, radius);
    if (s.hover || s.active) {
        roundedRect(vg, b.inset(0.5f), radius);
        nvgStrokeColor(vg, rgb(t.lineLit));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
    }
    // A page with its corner folded.
    const float h = std::min(14.0f, b.h - 10.0f), w = h * 0.78f, x = b.x + 10.0f, y = b.cy() - h * 0.5f, fold = w * 0.38f;
    const NVGcolor glyph = s.file == State::Missing || s.file == State::Failed ? rgb(t.alert)
                           : s.file == State::Ready                            ? rgb(t.accent)
                                                                               : rgb(t.textMuted);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x, y);
    nvgLineTo(vg, x + w - fold, y);
    nvgLineTo(vg, x + w, y + fold);
    nvgLineTo(vg, x + w, y + h);
    nvgLineTo(vg, x, y + h);
    nvgClosePath(vg);
    nvgMoveTo(vg, x + w - fold, y);
    nvgLineTo(vg, x + w - fold, y + fold);
    nvgLineTo(vg, x + w, y + fold);
    nvgStrokeColor(vg, glyph);
    nvgStrokeWidth(vg, 1.2f);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);

    // The state on the right, if the name still fits beside it; the name in
    // what is left. A factory file goes by its name, as the slot's menu lists it.
    const std::string name = files::isFactoryReference(s.text) ? std::string(s.text.substr(files::kFactoryPrefix.size()))
                                                               : files::fileName(s.text);
    const float left = x + w + 9.0f;
    const char* tag = nullptr;
    NVGcolor tagColour = rgb(t.textFaint);
    switch (s.file) {
        case State::Loading: tag = "Loading\xE2\x80\xA6"; break;
        case State::Missing: tag = "Missing"; tagColour = rgb(t.alert); break;
        case State::Failed: tag = "Can\xE2\x80\x99t load"; tagColour = rgb(t.alert); break;
        case State::Ready: tag = s.detail.empty() ? nullptr : s.detail.c_str(); break;
        case State::Empty: break;
    }
    float right = b.right() - 9.0f;
    if (tag) {
        const float tw = textWidth(vg, tag, 10.0f, font::kUi);
        const float named = name.empty() ? 0.0f : textWidth(vg, name.c_str(), 11.5f, font::kUi);
        if (tw < b.w * 0.45f && (s.file != State::Ready || left + named + 10.0f + tw <= right)) {
            label(vg, right, b.cy() + 0.5f, tag, 10.0f, tagColour, Align::Right);
            right -= tw + 10.0f;
        }
    }
    if (!name.empty())
        fittedLabel(vg, left, b.cy() + 0.5f, name.c_str(), 11.5f, 10.0f, right - left,
                    s.file == State::Missing ? rgb(t.textMuted) : rgb(t.text), Align::Left, false);
    else
        fittedLabel(vg, left, b.cy() + 0.5f, c.label ? c.label : "Choose or drop a file", 11.5f, 11.5f, right - left,
                    rgb(t.textFaint), Align::Left, false);
    if (s.focused) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

namespace {
void chevron(NVGcontext* vg, float cx, float cy, float direction, NVGcolor colour) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx + 2.0f * direction, cy - 4.5f);
    nvgLineTo(vg, cx - 2.5f * direction, cy);
    nvgLineTo(vg, cx + 2.0f * direction, cy + 4.5f);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, 1.6f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
}
}  // namespace

void presetBar(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const float radius = std::min(5.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled || !s.available) nvgGlobalAlpha(vg, 0.4f);
    const Rect body{b.x, b.y, Ui::presetPart(b, presetbar::kNext).right() - b.x, b.h};
    roundedRect(vg, body, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(), rgb(t.raised),
                                       lerp(rgb(t.raised), rgb(t.ground), 0.45f)));
    nvgFill(vg);
    roundedRect(vg, body.inset(0.5f), radius);
    nvgStrokeColor(vg, s.active ? rgba(t.accent, 0.85f) : rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    for (int part : {presetbar::kPrevious, presetbar::kName, presetbar::kNext}) {
        const Rect r = Ui::presetPart(b, part);
        const bool hot = s.hoverIndex == part;
        if (hot) {
            roundedRect(vg, r.inset(2.0f), radius - 1.5f);
            nvgFillColor(vg, rgba(t.text, 0.06f));
            nvgFill(vg);
        }
        if (part == presetbar::kName) {
            std::string name = s.available ? std::string(s.text) : std::string("Presets");
            if (name.empty()) name = "No preset";
            // A cut name keeps its mark of being modified.
            const char* mark = s.modified ? " *" : "";
            const float size = fitText(vg, name, 11.5f, 9.5f, r.w - 12.0f - textWidth(vg, mark, 11.5f, font::kUiBold),
                                       font::kUiBold);
            name += mark;
            label(vg, r.cx(), r.cy() + 0.5f, name.c_str(), size, rgb(t.text), Align::Center, true);
        } else {
            if (part == presetbar::kNext) hairline(vg, r.x, r.y + 5.0f, r.x, r.bottom() - 5.0f, rgba(t.line, 0.9f));
            else hairline(vg, r.right(), r.y + 5.0f, r.right(), r.bottom() - 5.0f, rgba(t.line, 0.9f));
            chevron(vg, r.cx(), r.cy(), part == presetbar::kPrevious ? 1.0f : -1.0f,
                    hot ? rgb(t.text) : rgb(t.textMuted));
        }
    }

    // A and B, the one in use lit.
    const Rect ab = Ui::presetPart(b, presetbar::kCompare);
    well(vg, t, ab, radius);
    for (int k = 0; k < 2; ++k) {
        const Rect half{ab.x + ab.w * 0.5f * static_cast<float>(k), ab.y, ab.w * 0.5f, ab.h};
        const bool on = s.slot == k;
        if (on) {
            roundedRect(vg, half.inset(2.5f), radius - 2.0f);
            nvgFillColor(vg, rgba(t.accent, 0.24f));
            nvgFill(vg);
            nvgStrokeColor(vg, rgba(t.accent, 0.75f));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
        label(vg, half.cx(), half.cy() + 0.5f, k == 0 ? "A" : "B", 11.0f,
              on ? rgb(t.text) : s.hoverIndex == presetbar::kCompare ? rgb(t.text) : rgb(t.textMuted),
              Align::Center, true);
    }
    if (s.focused) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void knob(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const float labelHeight = 20.0f;
    const float size = std::max(10.0f, std::min(b.w, b.h - labelHeight));
    const float cx = b.cx(), cy = b.y + size * 0.5f;
    const float r = size * 0.5f - 6.0f;  // track radius
    const float body = r - 8.0f;
    const bool hot = s.hover || s.active;
    auto angle = [](double n) { return kKnobStart + kKnobSweep * static_cast<float>(std::clamp(n, 0.0, 1.0)); };
    // A bipolar parameter's arc grows from zero, not from the bottom of the range.
    const float origin = s.def->bipolar() ? angle(toNormalized(*s.def, toHost(*s.def, 0.0))) : kKnobStart;
    const float value = angle(s.normalized);

    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    nvgLineCap(vg, NVG_ROUND);

    // Seat: a soft shadow and a dark disc the track sits in.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy + 2.0f, r + 7.0f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy + 2.0f, r - 4.0f, r + 7.0f, nvgRGBAf(0, 0, 0, 0.45f),
                                       nvgRGBAf(0, 0, 0, 0)));
    nvgFill(vg);

    arc(vg, cx, cy, r, kKnobStart, kKnobStart + kKnobSweep, rgb(t.knobTrack), 4.0f);
    // The lit value arc, with a faint bloom so it reads as light, not paint.
    arc(vg, cx, cy, r, origin, value, rgba(t.accent, hot ? 0.22f : 0.14f), 10.0f);
    arc(vg, cx, cy, r, origin, value, rgb(t.accent), 4.0f);
    if (s.modulated != s.normalized)
        arc(vg, cx, cy, r + 6.0f, value, angle(s.modulated), rgba(t.modulation, 0.95f), 2.0f);

    // Body: lit from above, with a bevelled rim.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, body + 1.5f);
    nvgFillColor(vg, rgba(t.ground, 0.9f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, body);
    nvgFillPaint(vg, nvgLinearGradient(vg, cx, cy - body, cx, cy + body,
                                       hot ? lerp(rgb(t.knobTop), rgb(t.text), 0.06f) : rgb(t.knobTop),
                                       rgb(t.knobBottom)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(t.knobRim, hot ? 1.0f : 0.8f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    arc(vg, cx, cy, body - 1.5f, kPi * 1.15f, kPi * 1.85f, rgba(t.text, 0.10f), 1.2f);

    // Pointer: a short tick at the rim, leaving the face to the value.
    const float ca = std::cos(value), sa = std::sin(value);
    const float tickIn = std::max(body * 0.66f, body - 11.0f);
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx + ca * tickIn, cy + sa * tickIn);
    nvgLineTo(vg, cx + ca * (body - 3.5f), cy + sa * (body - 3.5f));
    nvgStrokeColor(vg, hot ? rgb(t.text) : lerp(rgb(t.text), rgb(t.textMuted), 0.25f));
    nvgStrokeWidth(vg, 2.4f);
    nvgStroke(vg);

    // Value in the face, unit beneath it.
    if (!s.editing) {
        const ValueText v = valueText(s);
        const float numberSize = std::clamp(body * 0.36f, 9.5f, 15.0f), face = 2.0f * body - 6.0f;
        const float shift = v.unit[0] ? numberSize * 0.30f : 0.0f;
        fittedNumber(vg, cx, cy - shift, v.number, numberSize, 8.0f, face, rgb(t.text), Align::Center);
        if (v.unit[0])
            fittedLabel(vg, cx, cy + numberSize * 0.62f, v.unit, numberSize * 0.66f, numberSize * 0.66f, face,
                        rgb(t.textMuted), Align::Center, false);
    }

    fittedLabel(vg, cx, b.y + size + labelHeight * 0.45f, s.label(c), 11.0f, 9.0f, b.w,
                hot || s.focused ? rgb(t.text) : rgb(t.textMuted), Align::Center, true);

    if (s.focused) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r + 7.5f);
        nvgStrokeColor(vg, rgba(t.accent, 0.8f));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }
    nvgRestore(vg);
}

namespace {

// The value, in at most `width`; how wide it came out.
float sliderValueText(NVGcontext* vg, const Theme& t, const ControlState& s, float x, float y, Align a, float width) {
    if (s.editing) return 0.0f;
    char text[64];
    joined(valueText(s), text, sizeof(text));
    return fittedNumber(vg, x, y, text, 11.0f, 9.0f, width, rgb(t.text), a);
}

void thumb(NVGcontext* vg, const Theme& t, float x, float y, bool hot) {
    nvgBeginPath(vg);
    nvgCircle(vg, x, y + 1.0f, 9.0f);
    nvgFillPaint(vg, nvgRadialGradient(vg, x, y + 1.0f, 4.0f, 9.0f, nvgRGBAf(0, 0, 0, 0.5f), nvgRGBAf(0, 0, 0, 0)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, x, y, 7.0f);
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y - 7.0f, x, y + 7.0f, rgb(t.knobTop), rgb(t.knobBottom)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(t.knobRim, hot ? 1.0f : 0.8f));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, x, y, 2.2f);
    nvgFillColor(vg, rgb(t.accent));
    nvgFill(vg);
}

double originNormal(const ControlState& s) {
    return s.def->bipolar() ? toNormalized(*s.def, toHost(*s.def, 0.0)) : 0.0;
}

}  // namespace

void hslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool hot = s.hover || s.active;
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    // The value first; the name has what is left of the line.
    const float valueWidth = sliderValueText(vg, t, s, b.right(), b.y + 8.0f, Align::Right, b.w);
    fittedLabel(vg, b.x, b.y + 8.0f, s.label(c), 11.0f, 9.0f, b.w - valueWidth - 8.0f,
                hot || s.focused ? rgb(t.text) : rgb(t.textMuted), Align::Left, true);

    const float y = b.bottom() - 9.0f, x0 = b.x + 7.0f, x1 = b.right() - 7.0f;
    well(vg, t, {b.x, y - 3.0f, b.w, 6.0f}, 3.0f);
    const float xo = x0 + (x1 - x0) * static_cast<float>(originNormal(s));
    const float xv = x0 + (x1 - x0) * static_cast<float>(s.normalized);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, std::min(xo, xv), y - 2.0f, std::fabs(xv - xo), 4.0f, 2.0f);
    nvgFillColor(vg, rgb(t.accent));
    nvgFill(vg);
    if (s.modulated != s.normalized) {
        const float xm = x0 + (x1 - x0) * static_cast<float>(s.modulated);
        hairline(vg, xv, y - 7.0f, xm, y - 7.0f, rgb(t.modulation), 2.0f);
    }
    thumb(vg, t, xv, y, hot);
    if (s.focused) focusOutline(vg, t, b, 4.0f);
    nvgRestore(vg);
}

void vslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool hot = s.hover || s.active;
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    const float x = b.cx(), y0 = b.bottom() - 42.0f, y1 = b.y + 9.0f;
    well(vg, t, {x - 3.0f, y1 - 7.0f, 6.0f, y0 - y1 + 14.0f}, 3.0f);
    const float yo = y0 + (y1 - y0) * static_cast<float>(originNormal(s));
    const float yv = y0 + (y1 - y0) * static_cast<float>(s.normalized);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x - 2.0f, std::min(yo, yv), 4.0f, std::fabs(yv - yo), 2.0f);
    nvgFillColor(vg, rgb(t.accent));
    nvgFill(vg);
    if (s.modulated != s.normalized) {
        const float ym = y0 + (y1 - y0) * static_cast<float>(s.modulated);
        hairline(vg, x + 8.0f, yv, x + 8.0f, ym, rgb(t.modulation), 2.0f);
    }
    thumb(vg, t, x, yv, hot);
    sliderValueText(vg, t, s, x, b.bottom() - 25.0f, Align::Center, b.w);
    fittedLabel(vg, x, b.bottom() - 9.0f, s.label(c), 11.0f, 9.0f, b.w, hot || s.focused ? rgb(t.text) : rgb(t.textMuted),
                Align::Center, true);
    if (s.focused) focusOutline(vg, t, b, 4.0f);
    nvgRestore(vg);
}

void readout(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    char text[64];
    joined(valueText(s), text, sizeof(text));
    const float valueWidth = fittedNumber(vg, b.right(), b.y + 8.0f, text, 11.0f, 9.0f, b.w, rgb(t.text), Align::Right);
    fittedLabel(vg, b.x, b.y + 8.0f, s.label(c), 11.0f, 9.0f, b.w - valueWidth - 8.0f, rgb(t.textMuted), Align::Left,
                true);
    // The bar is lit in the meters' colour: it reports, it does not adjust.
    const Rect bar{b.x, b.bottom() - 6.0f, b.w, 6.0f};
    well(vg, t, bar, 3.0f);
    const float x0 = bar.x + 1.5f, x1 = bar.right() - 1.5f;
    const float xo = x0 + (x1 - x0) * static_cast<float>(originNormal(s));
    const float xv = x0 + (x1 - x0) * static_cast<float>(s.normalized);
    if (std::fabs(xv - xo) > 0.25f) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, std::min(xo, xv), bar.y + 1.5f, std::fabs(xv - xo), bar.h - 3.0f, 1.5f);
        nvgFillColor(vg, rgb(t.accent2));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

void toggle(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool on = s.value > s.def->hostMin();
    const bool hot = s.hover || s.active;
    const float radius = std::min(6.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    shadow(vg, b, radius, 6.0f, 0.4f);
    roundedRect(vg, b, radius);
    if (on)
        nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(), rgba(t.accent, hot ? 0.34f : 0.26f),
                                           rgba(t.accent, 0.12f)));
    else
        nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(),
                                           hot ? lerp(rgb(t.raised), rgb(t.text), 0.05f) : rgb(t.raised),
                                           lerp(rgb(t.raised), rgb(t.ground), 0.5f)));
    nvgFill(vg);
    roundedRect(vg, b.inset(0.5f), radius);
    nvgStrokeColor(vg, on ? rgba(t.accent, hot ? 0.95f : 0.7f) : hot ? rgb(t.lineLit) : rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    // A lamp on the left, then the label in the space that remains.
    const float lampX = b.x + std::min(14.0f, b.h * 0.5f + 2.0f), lampY = b.cy();
    nvgBeginPath(vg);
    nvgCircle(vg, lampX, lampY, 4.0f);
    nvgFillColor(vg, rgb(t.recess));
    nvgFill(vg);
    if (on) {
        nvgBeginPath(vg);
        nvgCircle(vg, lampX, lampY, 9.0f);
        nvgFillPaint(vg, nvgRadialGradient(vg, lampX, lampY, 1.0f, 9.0f, rgba(t.accent, 0.5f), rgba(t.accent, 0.0f)));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, lampX, lampY, 2.8f);
        nvgFillColor(vg, rgb(t.accent));
        nvgFill(vg);
    } else {
        nvgBeginPath(vg);
        nvgCircle(vg, lampX, lampY, 2.6f);
        nvgFillColor(vg, rgb(hot ? t.textMuted : t.textFaint));
        nvgFill(vg);
    }
    const float left = lampX + 9.0f, avail = b.right() - left - 8.0f;
    fittedLabel(vg, left + avail * 0.5f, b.cy() + 0.5f, s.label(c), 11.5f, 9.0f, avail,
                on || hot ? rgb(t.text) : rgb(t.textMuted), Align::Center, true);
    if (s.focused) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void segmented(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const ParamDef& p = *s.def;
    const int n = static_cast<int>(std::lround(p.hostMax() - p.hostMin())) + 1;
    const int selected = static_cast<int>(std::lround(s.value - p.hostMin()));
    const float radius = std::min(6.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, b, radius);
    const float w = b.w / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        const Rect seg{b.x + w * static_cast<float>(i), b.y, w, b.h};
        const bool isSelected = i == selected, isHover = i == s.hoverIndex;
        if (isSelected) {
            roundedRect(vg, seg.inset(2.5f), radius - 2.0f);
            nvgFillPaint(vg, nvgLinearGradient(vg, seg.x, seg.y, seg.x, seg.bottom(), rgba(t.accent, 0.30f),
                                               rgba(t.accent, 0.16f)));
            nvgFill(vg);
            nvgStrokeColor(vg, rgba(t.accent, 0.75f));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        } else if (isHover) {
            roundedRect(vg, seg.inset(2.5f), radius - 2.0f);
            nvgFillColor(vg, rgba(t.raised, 0.9f));
            nvgFill(vg);
        }
        if (i > 0 && !isSelected && i - 1 != selected)
            hairline(vg, seg.x, seg.y + 6.0f, seg.x, seg.bottom() - 6.0f, rgba(t.line, 0.9f));
        char scratch[16];
        const char* text = optionLabel(p, i, scratch, sizeof(scratch));
        fittedLabel(vg, seg.cx(), seg.cy() + 0.5f, text, 11.0f, 8.5f, w - 10.0f,
                    isSelected ? rgb(t.text) : isHover ? lerp(rgb(t.textMuted), rgb(t.text), 0.5f) : rgb(t.textMuted),
                    Align::Center, true);
    }
    if (s.focused) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void dropdown(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const bool hot = s.hover || s.active;
    const float radius = std::min(5.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    roundedRect(vg, b, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(),
                                       hot ? lerp(rgb(t.raised), rgb(t.text), 0.05f) : rgb(t.raised),
                                       lerp(rgb(t.raised), rgb(t.ground), 0.45f)));
    nvgFill(vg);
    roundedRect(vg, b.inset(0.5f), radius);
    nvgStrokeColor(vg, s.active ? rgba(t.accent, 0.85f) : hot ? rgb(t.lineLit) : rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    const ValueText v = valueText(s);
    fittedLabel(vg, b.x + 10.0f, b.cy() + 0.5f, v.number, 11.5f, 9.0f, b.w - 34.0f, rgb(t.text), Align::Left, true);
    const float ax = b.right() - 13.0f, ay = b.cy();
    nvgBeginPath(vg);
    nvgMoveTo(vg, ax - 4.0f, ay - 2.0f);
    nvgLineTo(vg, ax, ay + 2.5f);
    nvgLineTo(vg, ax + 4.0f, ay - 2.0f);
    nvgStrokeColor(vg, hot ? rgb(t.text) : rgb(t.textMuted));
    nvgStrokeWidth(vg, 1.5f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
    if (s.focused && !s.active) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void overlays(NVGcontext* vg, const Theme& t, const Ui& ui) {
    const Ui::Menu& m = ui.menu();
    if (m.open) {
        nvgSave(vg);
        shadow(vg, m.bounds, 6.0f, 18.0f, 0.6f);
        roundedRect(vg, m.bounds, 6.0f);
        nvgFillColor(vg, rgb(t.surface));
        nvgFill(vg);
        roundedRect(vg, m.bounds.inset(0.5f), 6.0f);
        nvgStrokeColor(vg, rgb(t.lineLit));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        nvgScissor(vg, m.bounds.x, m.bounds.y + 2.0f, m.bounds.w, m.bounds.h - 4.0f);
        for (int i = m.first; i < m.first + m.visible && i < m.count(); ++i) {
            const MenuItem& item = m.items[static_cast<size_t>(i)];
            const Rect row = m.row(i).inset(4.0f, 1.0f);
            if (item.kind == MenuItem::Kind::Separator) {
                hairline(vg, row.x + 6.0f, std::round(row.cy()) + 0.5f, row.right() - 6.0f,
                         std::round(row.cy()) + 0.5f, rgb(t.line));
                continue;
            }
            if (item.kind == MenuItem::Kind::Header) {
                caption(vg, row.x + 8.0f, row.cy() + 1.0f, item.label.c_str(), rgb(t.textFaint));
                continue;
            }
            if (i == m.hover) {
                roundedRect(vg, row, 4.0f);
                nvgFillColor(vg, rgba(t.accent, 0.18f));
                nvgFill(vg);
            }
            if (item.checked) {
                nvgBeginPath(vg);
                nvgCircle(vg, row.x + 10.0f, row.cy(), 2.6f);
                nvgFillColor(vg, rgb(t.accent));
                nvgFill(vg);
            }
            const NVGcolor colour = !item.enabled                    ? rgb(t.textFaint)
                                    : i == m.hover || item.checked ? rgb(t.text)
                                                                   : rgb(t.textMuted);
            fittedLabel(vg, row.x + 20.0f, row.cy() + 0.5f, item.label.c_str(), 11.5f, 9.0f, row.w - 28.0f, colour,
                        Align::Left, item.checked);
        }
        nvgResetScissor(vg);
        // More above or below: a small arrow at that edge.
        for (int edge = 0; edge < 2; ++edge) {
            if (edge == 0 ? !m.scrollsUp() : !m.scrollsDown()) continue;
            const float cx = m.bounds.right() - 14.0f;
            const float cy = edge == 0 ? m.bounds.y + 7.0f : m.bounds.bottom() - 7.0f;
            const float d = edge == 0 ? -1.0f : 1.0f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 4.0f, cy - 2.0f * d);
            nvgLineTo(vg, cx, cy + 2.0f * d);
            nvgLineTo(vg, cx + 4.0f, cy - 2.0f * d);
            nvgClosePath(vg);
            nvgFillColor(vg, rgb(t.textMuted));
            nvgFill(vg);
        }
        nvgRestore(vg);
    }

    const Ui::TextEdit& e = ui.textEdit();
    if (e.open) {
        const Rect r = e.bounds;
        nvgSave(vg);
        shadow(vg, r, 5.0f, 10.0f, 0.5f);
        roundedRect(vg, r, 5.0f);
        nvgFillColor(vg, rgb(t.recess));
        nvgFill(vg);
        roundedRect(vg, r.inset(0.5f), 5.0f);
        nvgStrokeColor(vg, e.rejected ? rgb(t.alert) : rgb(t.accent));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);

        // A value is centred in Plex Mono; text is left-aligned in Inter and
        // slides along to keep the caret in view (Ui::textStart).
        textEntryFont(vg, e.ascii);
        const auto widthTo = [&](int bytes) {
            return bytes > 0 ? nvgTextBounds(vg, 0, 0, e.buffer, e.buffer + bytes, nullptr) : 0.0f;
        };
        const float x = Ui::textStart(e, widthTo(e.length));
        const float y = r.cy() + 0.5f;
        nvgScissor(vg, r.x + 3.0f, r.y, r.w - 6.0f, r.h);
        if (e.selected()) {
            const float from = x + widthTo(e.from()), to = x + widthTo(e.to());
            roundedRect(vg, {from - 1.0f, r.y + 4.0f, to - from + 2.0f, r.h - 8.0f}, 2.0f);
            nvgFillColor(vg, rgba(t.accent, 0.30f));
            nvgFill(vg);
        }
        nvgFillColor(vg, rgb(t.text));
        nvgText(vg, x, y, e.buffer, e.buffer + e.length);
        if (e.caretOn && !e.selected()) {
            nvgBeginPath(vg);
            nvgRect(vg, std::round(x + widthTo(e.caret)), r.y + 5.0f, 1.0f, r.h - 10.0f);
            nvgFillColor(vg, rgb(t.accent));
            nvgFill(vg);
        }
        nvgResetScissor(vg);
        nvgRestore(vg);
    }

    const Ui::Tooltip& tip = ui.tooltip();
    if (tip.open && !m.open && !e.open) {
        // At most 220 wide and inside the editor: the body wraps, the title
        // and the last line that fits end in an ellipsis if they must.
        nvgSave(vg);
        const float pad = 8.0f, size = 11.0f;
        const float widest = std::max(0.0f, ui.designWidth() - 8.0f - pad * 2.0f);
        const float wrap = std::min(220.0f, widest);
        nvgFontFace(vg, font::kUi);
        nvgFontSize(vg, size);
        float lineHeight = 0.0f;
        nvgTextMetrics(vg, nullptr, nullptr, &lineHeight);
        lineHeight *= 1.3f;
        const float titleHeight = tip.title.empty() ? 0.0f : 16.0f;
        const int room = std::max(1, static_cast<int>((ui.designHeight() - 8.0f - pad * 2.0f - titleHeight) / lineHeight));
        NVGtextRow rows[24];
        const char* end = tip.text.c_str() + tip.text.size();
        const int count = tip.text.empty() ? 0 : nvgTextBreakLines(vg, tip.text.c_str(), end, wrap, rows, 24);
        const int shown = std::min(count, room);
        std::vector<std::string> lines;
        float width = 0.0f;
        for (int i = 0; i < shown; ++i) {
            lines.emplace_back(rows[i].start, rows[i].end);
            if (i == shown - 1 && rows[i].next < end) {  // more than fits
                lines.back() += "\xE2\x80\xA6";
                fitText(vg, lines.back(), size, size, wrap, font::kUi);
            }
            width = std::max(width, textWidth(vg, lines.back().c_str(), size, font::kUi));
        }
        std::string title = tip.title;
        if (!title.empty()) {
            fitText(vg, title, size, size, widest, font::kUiBold);
            width = std::max(width, textWidth(vg, title.c_str(), size, font::kUiBold));
        }
        const Rect a = tip.anchor;
        Rect box{0, 0, width + pad * 2.0f, lineHeight * static_cast<float>(shown) + titleHeight + pad * 2.0f};
        box.x = std::clamp(a.cx() - box.w * 0.5f, 4.0f, std::max(4.0f, ui.designWidth() - box.w - 4.0f));
        box.y = a.bottom() + 6.0f;
        if (box.bottom() > ui.designHeight() - 4.0f) box.y = std::max(4.0f, a.y - 6.0f - box.h);
        shadow(vg, box, 5.0f, 12.0f, 0.5f);
        roundedRect(vg, box, 5.0f);
        nvgFillColor(vg, rgb(t.raised));
        nvgFill(vg);
        roundedRect(vg, box.inset(0.5f), 5.0f);
        nvgStrokeColor(vg, rgb(t.lineLit));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        if (!title.empty()) label(vg, box.x + pad, box.y + pad + 6.0f, title.c_str(), size, rgb(t.text), Align::Left, true);
        nvgFontFace(vg, font::kUi);
        nvgFontSize(vg, size);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, rgb(tip.title.empty() ? t.text : t.textMuted));
        for (int i = 0; i < shown; ++i)
            nvgText(vg, box.x + pad, box.y + pad + titleHeight + lineHeight * static_cast<float>(i),
                    lines[static_cast<size_t>(i)].c_str(), nullptr);
        nvgRestore(vg);
    }
}

void dropTarget(NVGcontext* vg, const Theme& t, Rect r) {
    nvgSave(vg);
    roundedRect(vg, r.inset(3.0f), t.radius);
    nvgStrokeColor(vg, rgba(t.accent, 0.25f));
    nvgStrokeWidth(vg, 6.0f);
    nvgStroke(vg);
    nvgStrokeColor(vg, rgba(t.accent, 0.9f));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
    nvgRestore(vg);
}

// --- meters ------------------------------------------------------------------------

float meterPosition(float db) {
    const float lin = std::clamp((db - PeakMeter::kFloorDb) / (6.0f - PeakMeter::kFloorDb), 0.0f, 1.0f);
    return std::pow(lin, 1.6f);  // more of the travel near the top, where it matters
}

void meter(NVGcontext* vg, const Theme& t, Rect r, const PeakMeter* channels, int count, const RmsMeter* rms) {
    if (count <= 0) return;
    nvgSave(vg);
    const float lamp = 6.0f, gap = 2.0f;
    const Rect bars{r.x, r.y + lamp + 3.0f, r.w, r.h - lamp - 3.0f};
    const float w = (bars.w - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
    bool clipped = false;
    for (int c = 0; c < count; ++c) {
        const PeakMeter& m = channels[c];
        clipped = clipped || m.clipped();
        const Rect bar{bars.x + (w + gap) * static_cast<float>(c), bars.y, w, bars.h};
        well(vg, t, bar, 2.0f);
        const Rect inner = bar.inset(1.5f);
        const float level = meterPosition(m.levelDb());
        if (level > 0.0f) {
            const float top = inner.bottom() - inner.h * level;
            const float zero = inner.bottom() - inner.h * meterPosition(0.0f);
            nvgBeginPath(vg);
            nvgRect(vg, inner.x, top, inner.w, inner.bottom() - top);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, inner.bottom(), 0, inner.y, rgba(t.accent2, rms ? 0.3f : 0.75f),
                                               withAlpha(lerp(rgb(t.accent2), rgb(t.text), 0.3f), rms ? 0.4f : 1.0f)));
            nvgFill(vg);
            if (rms) {
                const float body = inner.bottom() - inner.h * meterPosition(rms[c].levelDb());
                nvgBeginPath(vg);
                nvgRect(vg, inner.x, body, inner.w, inner.bottom() - body);
                nvgFillPaint(vg, nvgLinearGradient(vg, 0, inner.bottom(), 0, inner.y, rgba(t.accent2, 0.8f),
                                                   lerp(rgb(t.accent2), rgb(t.text), 0.3f)));
                nvgFill(vg);
            }
            if (top < zero) {  // over full scale
                nvgBeginPath(vg);
                nvgRect(vg, inner.x, top, inner.w, zero - top);
                nvgFillColor(vg, rgb(t.alert));
                nvgFill(vg);
            }
        }
        if (m.holdDb() > PeakMeter::kFloorDb) {
            const float y = inner.bottom() - inner.h * meterPosition(m.holdDb());
            nvgBeginPath(vg);
            nvgRect(vg, inner.x, std::round(y) - 0.75f, inner.w, 1.5f);
            nvgFillColor(vg, m.holdDb() >= 0.0f ? rgb(t.alert) : rgb(t.text));
            nvgFill(vg);
        }
    }
    roundedRect(vg, {r.x, r.y, r.w, lamp}, 2.0f);
    nvgFillColor(vg, clipped ? rgb(t.alert) : rgb(t.recess));
    nvgFill(vg);
    nvgRestore(vg);
}

}  // namespace supple::gui::widgets

namespace supple::gui::widgets {

// --- more controls ----------------------------------------------------------------

void button(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    if (c.widget == Widget::Momentary) return toggle(vg, t, c, s);
    const Rect b = c.bounds;
    const bool hot = s.hover || s.active;
    const float radius = std::min(6.0f, b.h * 0.5f);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    shadow(vg, b, radius, 6.0f, 0.4f);
    roundedRect(vg, b, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, b.x, b.y, b.x, b.bottom(),
                                       hot ? lerp(rgb(t.raised), rgb(t.text), 0.06f) : rgb(t.raised),
                                       lerp(rgb(t.raised), rgb(t.ground), 0.5f)));
    nvgFill(vg);
    roundedRect(vg, b.inset(0.5f), radius);
    nvgStrokeColor(vg, hot ? rgb(t.lineLit) : rgb(t.line));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
    fittedLabel(vg, b.cx(), b.cy() + 0.5f, s.label(c), 11.5f, 9.0f, b.w - 14.0f,
                hot ? rgb(t.text) : lerp(rgb(t.textMuted), rgb(t.text), 0.4f), Align::Center, true);
    if (s.focused) focusOutline(vg, t, b, radius);
    nvgRestore(vg);
}

void xyPad(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const Rect area = b.inset(8.0f);
    const bool hot = s.hover || s.active;
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, b, 5.0f);
    for (int k = 1; k < 4; ++k) {
        const float fx = area.x + area.w * static_cast<float>(k) / 4.0f;
        const float fy = area.y + area.h * static_cast<float>(k) / 4.0f;
        hairline(vg, std::floor(fx) + 0.5f, area.y, std::floor(fx) + 0.5f, area.bottom(), rgba(t.line, k == 2 ? 0.9f : 0.5f));
        hairline(vg, area.x, std::floor(fy) + 0.5f, area.right(), std::floor(fy) + 0.5f, rgba(t.line, k == 2 ? 0.9f : 0.5f));
    }
    const ControlState::Slot& sx = s.slots[0];
    const ControlState::Slot& sy = s.slots[1];
    const float px = area.x + area.w * static_cast<float>(sx.normalized);
    const float py = area.bottom() - area.h * static_cast<float>(sy.def ? sy.normalized : 0.5);
    hairline(vg, px, area.y, px, area.bottom(), rgba(t.accent, hot ? 0.45f : 0.25f));
    hairline(vg, area.x, py, area.right(), py, rgba(t.accent, hot ? 0.45f : 0.25f));
    // Where host modulation takes it, if anywhere.
    if (sx.modulated != sx.normalized || (sy.def && sy.modulated != sy.normalized)) {
        const float mx = area.x + area.w * static_cast<float>(sx.modulated);
        const float my = area.bottom() - area.h * static_cast<float>(sy.def ? sy.modulated : 0.5);
        hairline(vg, px, py, mx, my, rgba(t.modulation, 0.8f), 1.5f);
        nvgBeginPath(vg);
        nvgCircle(vg, mx, my, 3.5f);
        nvgFillColor(vg, rgb(t.modulation));
        nvgFill(vg);
    }
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, hot ? 13.0f : 11.0f);
    nvgFillPaint(vg, nvgRadialGradient(vg, px, py, 2.0f, hot ? 13.0f : 11.0f, rgba(t.accent, 0.45f), rgba(t.accent, 0.0f)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, 6.0f);
    nvgFillColor(vg, rgb(t.accent));
    nvgFill(vg);
    nvgStrokeColor(vg, rgb(t.ground));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
    // Each axis's name and value, in the corners they belong to.
    char text[64];
    if (sx.def) {
        ControlState one = s;
        joined(valueText(one), text, sizeof(text));
        fittedLabel(vg, area.right(), area.bottom() - 7.0f, sx.def->displayLabel(), 10.0f, 8.5f, area.w, rgb(t.textMuted),
                    Align::Right, true);
        fittedNumber(vg, area.right(), area.bottom() - 20.0f, text, 10.0f, 8.5f, area.w, rgb(t.text), Align::Right);
    }
    if (sy.def) {
        ControlState other;
        other.def = sy.def;
        other.value = sy.value;
        joined(valueText(other), text, sizeof(text));
        fittedLabel(vg, area.x, area.y + 6.0f, sy.def->displayLabel(), 10.0f, 8.5f, area.w, rgb(t.textMuted), Align::Left,
                    true);
        fittedNumber(vg, area.x, area.y + 19.0f, text, 10.0f, 8.5f, area.w, rgb(t.text), Align::Left);
    }
    if (s.focused) focusOutline(vg, t, b, 5.0f);
    nvgRestore(vg);
}

void tabs(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const int n = static_cast<int>(c.pages.size());
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    hairline(vg, b.x, b.bottom() - 0.5f, b.right(), b.bottom() - 0.5f, rgb(t.line));
    for (int k = 0; k < n; ++k) {
        const Rect r = Ui::tab(b, k, n);
        const bool on = k == c.selected, hot = k == s.hoverIndex;
        if (hot && !on) {
            roundedRect(vg, r.inset(2.0f, 3.0f), 4.0f);
            nvgFillColor(vg, rgba(t.text, 0.05f));
            nvgFill(vg);
        }
        fittedLabel(vg, r.cx(), r.cy(), c.pages[static_cast<size_t>(k)].c_str(), 11.5f, 9.0f, r.w - 12.0f,
                    on ? rgb(t.text) : hot ? lerp(rgb(t.textMuted), rgb(t.text), 0.5f) : rgb(t.textMuted), Align::Center,
                    true);
        if (on) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, r.x + 10.0f, b.bottom() - 2.5f, r.w - 20.0f, 2.5f, 1.25f);
            nvgFillColor(vg, rgb(t.accent));
            nvgFill(vg);
        }
    }
    if (s.focused) focusOutline(vg, t, b, 4.0f);
    nvgRestore(vg);
}

void list(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, b, 5.0f);
    if (!c.source) {
        nvgRestore(vg);
        return;
    }
    const int n = c.source->size(), rows = Ui::listRows(c), selected = c.source->selected();
    nvgSave(vg);
    nvgIntersectScissor(vg, b.x + 2.0f, b.y + 2.0f, b.w - 4.0f, b.h - 4.0f);
    const bool scrolls = n > rows;
    for (int r = c.scroll; r < std::min(n, c.scroll + rows + 1); ++r) {
        Rect row = Ui::listRow(c, r);
        if (scrolls) row.w -= 8.0f;
        const std::string text = c.source->item(r);
        if (c.source->header(r)) {
            caption(vg, row.x + 6.0f, row.cy() + 1.0f, text.c_str(), rgb(t.textFaint), Align::Left, 9.0f);
            continue;
        }
        const bool on = r == selected, hot = r == s.hoverIndex;
        if (on || hot) {
            roundedRect(vg, row.inset(0.0f, 1.0f), 4.0f);
            nvgFillColor(vg, on ? rgba(t.accent, 0.22f) : rgba(t.text, 0.05f));
            nvgFill(vg);
        }
        fittedLabel(vg, row.x + 8.0f, row.cy() + 0.5f, text.c_str(), 11.5f, 9.0f, row.w - 16.0f,
                    on ? rgb(t.text) : rgb(t.textMuted), Align::Left, on);
    }
    nvgRestore(vg);
    if (scrolls) {
        const Rect track{b.right() - 8.0f, b.y + 5.0f, 3.0f, b.h - 10.0f};
        const float thumb = std::max(16.0f, track.h * static_cast<float>(rows) / static_cast<float>(n));
        const float at = track.y + (track.h - thumb) * static_cast<float>(c.scroll) / static_cast<float>(std::max(1, n - rows));
        roundedRect(vg, {track.x, at, track.w, thumb}, 1.5f);
        nvgFillColor(vg, rgba(t.textMuted, 0.6f));
        nvgFill(vg);
    }
    if (s.focused) focusOutline(vg, t, b, 5.0f);
    nvgRestore(vg);
}

void keyboard(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, c.bounds.inset(-3.0f), 5.0f);
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = c.lowKey; k <= c.highKey; ++k) {
            const bool black = Ui::blackKey(k);
            if (black != (pass == 1)) continue;
            const Rect r = Ui::keyRect(c, k);
            const bool held = s.keyHeld(k), hot = k == s.hoverIndex;
            const Rect face = black ? r : r.inset(0.75f, 0.0f);
            nvgBeginPath(vg);
            nvgRoundedRectVarying(vg, face.x, face.y, face.w, face.h, 0.0f, 0.0f, 3.0f, 3.0f);
            if (held) {
                nvgFillPaint(vg, nvgLinearGradient(vg, 0, face.y, 0, face.bottom(), rgba(t.accent, 0.7f), rgb(t.accent)));
            } else if (black) {
                nvgFillPaint(vg, nvgLinearGradient(vg, 0, face.y, 0, face.bottom(), rgb(t.recess),
                                                   hot ? lerp(rgb(t.recess), rgb(t.text), 0.15f) : lerp(rgb(t.recess), rgb(t.raised), 0.6f)));
            } else {
                const NVGcolor top = lerp(rgb(t.text), rgb(t.raised), 0.25f);
                nvgFillPaint(vg, nvgLinearGradient(vg, 0, face.y, 0, face.bottom(), hot ? rgb(t.text) : top,
                                                   lerp(top, rgb(t.textMuted), 0.35f)));
            }
            nvgFill(vg);
            if (!black && k % 12 == 0 && face.w >= 10.0f) {
                char name[16];
                std::snprintf(name, sizeof(name), "C%d", k / 12 - 1);
                label(vg, face.cx(), face.bottom() - 8.0f, name, std::min(9.0f, face.w * 0.6f),
                      held ? rgb(t.ground) : rgba(t.ground, 0.7f), Align::Center, true);
            }
        }
    }
    nvgRestore(vg);
}

void envelope(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s) {
    const Rect b = c.bounds;
    const Ui::EnvelopeShape e = Ui::envelopeShape(c, s);
    nvgSave(vg);
    if (!c.enabled) nvgGlobalAlpha(vg, 0.4f);
    well(vg, t, b, 5.0f);
    auto trace = [&] {
        nvgBeginPath(vg);
        nvgMoveTo(vg, e.x[0], e.y[0]);
        for (int k = 1; k < 5; ++k) nvgLineTo(vg, e.x[k], e.y[k]);
    };
    trace();
    nvgLineTo(vg, e.x[0], e.y[0]);
    nvgClosePath(vg);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, b.y, 0, b.bottom(), rgba(t.accent, 0.22f), rgba(t.accent, 0.04f)));
    nvgFill(vg);
    trace();
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, rgb(t.accent));
    nvgStrokeWidth(vg, 2.0f);
    nvgStroke(vg);
    // A handle per stage; the one under the pointer or in hand is lit.
    const int lit = s.activeIndex >= 0 ? s.activeIndex : s.hoverIndex;
    for (int h = 0; h < 3; ++h) {
        if (!e.has[h]) continue;
        const int point = h == 2 ? 4 : h + 1;
        nvgBeginPath(vg);
        nvgCircle(vg, e.x[point], e.y[point], h == lit ? 6.5f : 5.0f);
        nvgFillColor(vg, h == lit ? rgb(t.accent) : rgb(t.recess));
        nvgFill(vg);
        nvgStrokeColor(vg, h == lit ? rgb(t.ground) : rgb(t.accent));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }
    if (lit >= 0) {
        // Its parameters' names and values, top right.
        std::string text;
        for (int slot : {lit == 0 ? 0 : lit == 1 ? 1 : 3, lit == 1 ? 2 : -1}) {
            if (slot < 0 || !s.slots[slot].def) continue;
            ControlState one;
            one.def = s.slots[slot].def;
            one.value = s.slots[slot].value;
            char value[64];
            joined(valueText(one), value, sizeof(value));
            if (!text.empty()) text += "   ";
            text += std::string(one.def->displayLabel()) + " " + value;
        }
        fittedLabel(vg, b.right() - 10.0f, b.y + 12.0f, text.c_str(), 10.5f, 9.0f, b.w - 20.0f, rgb(t.text), Align::Right,
                    true);
    }
    if (s.focused) focusOutline(vg, t, b, 5.0f);
    nvgRestore(vg);
}

// --- graphs ------------------------------------------------------------------------------

void frequencyGrid(NVGcontext* vg, const Theme& t, Rect r, const Axis& hz, bool labels) {
    const double lo = std::min(hz.lo, hz.hi), hi = std::max(hz.lo, hz.hi);
    for (double decade = 1.0; decade <= hi; decade *= 10.0) {
        for (double m : {1.0, 2.0, 5.0}) {
            const double f = decade * m;
            if (f < lo || f > hi) continue;
            const float x = std::floor(hz.at(f)) + 0.5f;
            const bool major = m == 1.0;
            hairline(vg, x, r.y, x, r.bottom(), rgba(major ? t.lineLit : t.line, major ? 0.8f : 0.6f));
            if (!labels) continue;
            char text[16];
            if (f >= 1000.0) std::snprintf(text, sizeof(text), "%gk", f / 1000.0);
            else std::snprintf(text, sizeof(text), "%g", f);
            number(vg, x, r.bottom() + 14.0f, text, 9.5f, rgb(t.textFaint), Align::Center, false);
        }
    }
}

void levelGrid(NVGcontext* vg, const Theme& t, Rect r, const Axis& db, double step, bool labels) {
    if (step <= 0.0) return;
    const double lo = std::min(db.lo, db.hi), hi = std::max(db.lo, db.hi);
    for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9; v += step) {
        const float y = std::floor(db.at(v)) + 0.5f;
        hairline(vg, r.x, y, r.right(), y, std::fabs(v) < 1e-9 ? rgba(t.lineLit, 1.0f) : rgba(t.line, 0.7f));
        if (!labels) continue;
        char text[16];
        if (std::fabs(v) < 1e-9) std::snprintf(text, sizeof(text), "0");
        else std::snprintf(text, sizeof(text), "%+g", v);
        number(vg, r.x - 10.0f, y, text, 9.5f, rgb(t.textFaint), Align::Right, false);
    }
}

void plot(NVGcontext* vg, Rect r, const Axis& x, const Axis& y, const std::function<double(double)>& f,
          NVGcolor colour, float width, const double* fillTo) {
    nvgSave(vg);
    nvgIntersectScissor(vg, r.x, r.y, r.w, r.h);
    const int points = std::max(2, static_cast<int>(r.w / 2.0f) + 1);
    auto trace = [&] {
        nvgBeginPath(vg);
        for (int i = 0; i < points; ++i) {
            const float px = r.x + r.w * static_cast<float>(i) / static_cast<float>(points - 1);
            const double v = f(x.value(px));
            const float py = std::isfinite(v) ? y.at(v) : y.from;
            if (i == 0) nvgMoveTo(vg, px, py);
            else nvgLineTo(vg, px, py);
        }
    };
    if (fillTo) {
        trace();
        nvgLineTo(vg, r.right(), y.at(*fillTo));
        nvgLineTo(vg, r.x, y.at(*fillTo));
        nvgClosePath(vg);
        nvgFillColor(vg, withAlpha(colour, colour.a * 0.13f));
        nvgFill(vg);
    }
    trace();
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, withAlpha(colour, colour.a * 0.18f));
    nvgStrokeWidth(vg, width * 3.5f);
    nvgStroke(vg);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
    nvgRestore(vg);
}

void trace(NVGcontext* vg, Rect r, const ValueHistory& values, const Axis& y, NVGcolor colour, float width) {
    const size_t n = values.size();
    nvgSave(vg);
    nvgIntersectScissor(vg, r.x, r.y, r.w, r.h);
    nvgBeginPath(vg);
    for (size_t k = 0; k < n; ++k) {
        const float px = r.x + r.w * static_cast<float>(k) / static_cast<float>(n - 1);
        const float py = y.clamped(values.at(n - 1 - k));
        if (k == 0) nvgMoveTo(vg, px, py);
        else nvgLineTo(vg, px, py);
    }
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
    nvgRestore(vg);
}

void spectrum(NVGcontext* vg, Rect r, const Spectrum& s, const Axis& hz, const Axis& db, NVGcolor colour) {
    nvgSave(vg);
    nvgIntersectScissor(vg, r.x, r.y, r.w, r.h);
    const float step = 2.0f;
    auto shape = [&] {
        nvgBeginPath(vg);
        bool first = true;
        for (float px = r.x; px <= r.right() + 0.01f; px += step) {
            const double lo = hz.value(px - step * 0.5f), hi = hz.value(px + step * 0.5f);
            const float py = db.clamped(s.peak(std::min(lo, hi), std::max(lo, hi)));
            if (first) nvgMoveTo(vg, px, py);
            else nvgLineTo(vg, px, py);
            first = false;
        }
    };
    shape();
    nvgLineTo(vg, r.right(), r.bottom());
    nvgLineTo(vg, r.x, r.bottom());
    nvgClosePath(vg);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, r.y, 0, r.bottom(), withAlpha(colour, colour.a * 0.28f),
                                       withAlpha(colour, colour.a * 0.04f)));
    nvgFill(vg);
    shape();
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, withAlpha(colour, colour.a * 0.8f));
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);
    nvgRestore(vg);
}

void waveform(NVGcontext* vg, Rect r, const AudioHistory& audio, Signal signal, double seconds, double sampleRate,
              NVGcolor colour) {
    const int columns = std::max(2, static_cast<int>(r.w));
    const double total = std::clamp(seconds * sampleRate, 2.0, static_cast<double>(AudioHistory::kFrames - 1));
    const double perColumn = total / columns;
    std::vector<float> high(static_cast<size_t>(columns)), low(static_cast<size_t>(columns));
    for (int col = 0; col < columns; ++col) {
        // Column 0 is the oldest; the newest sample is at the right edge.
        const size_t newest = static_cast<size_t>(std::lround((columns - 1 - col) * perColumn));
        const size_t oldest = static_cast<size_t>(std::lround((columns - col) * perColumn));
        float lo = 1.0f, hi = -1.0f;
        for (size_t age = newest; age < std::max(oldest, newest + 1); ++age) {
            const float v = audio.at(signal, age);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        high[static_cast<size_t>(col)] = std::clamp(hi, -1.0f, 1.0f);
        low[static_cast<size_t>(col)] = std::clamp(lo, -1.0f, 1.0f);
    }
    const float mid = r.cy(), half = r.h * 0.5f;
    nvgSave(vg);
    nvgIntersectScissor(vg, r.x, r.y, r.w, r.h);
    nvgBeginPath(vg);
    for (int col = 0; col < columns; ++col) {
        const float px = r.x + r.w * static_cast<float>(col) / static_cast<float>(columns - 1);
        const float py = mid - half * high[static_cast<size_t>(col)];
        if (col == 0) nvgMoveTo(vg, px, py);
        else nvgLineTo(vg, px, py);
    }
    for (int col = columns - 1; col >= 0; --col) {
        const float px = r.x + r.w * static_cast<float>(col) / static_cast<float>(columns - 1);
        nvgLineTo(vg, px, mid - half * low[static_cast<size_t>(col)] + 0.5f);
    }
    nvgClosePath(vg);
    nvgFillColor(vg, colour);
    nvgFill(vg);
    nvgRestore(vg);
}

void oscilloscope(NVGcontext* vg, Rect r, const AudioHistory& audio, Signal signal, double seconds,
                  double sampleRate, NVGcolor colour, float width) {
    const size_t window = static_cast<size_t>(std::clamp(seconds * sampleRate, 2.0, AudioHistory::kFrames / 2.0));
    // The newest rising zero crossing with a whole window after it, searched
    // back by up to another window (a period of anything above 1 / seconds).
    size_t start = window - 1;  // age of the first sample drawn
    for (size_t age = window - 1; age + 1 < std::min(AudioHistory::kFrames, 2 * window); ++age)
        if (audio.at(signal, age + 1) <= 0.0f && audio.at(signal, age) > 0.0f) {
            start = age;
            break;
        }
    const float mid = r.cy(), half = r.h * 0.5f;
    const int points = std::max(2, static_cast<int>(std::min<double>(static_cast<double>(window), r.w * 2.0)));
    nvgSave(vg);
    nvgIntersectScissor(vg, r.x, r.y, r.w, r.h);
    nvgBeginPath(vg);
    for (int i = 0; i < points; ++i) {
        const double along = static_cast<double>(i) / (points - 1) * static_cast<double>(window - 1);
        const size_t age = start - std::min(start, static_cast<size_t>(along));
        const float px = r.x + r.w * static_cast<float>(i) / static_cast<float>(points - 1);
        const float py = mid - half * std::clamp(audio.at(signal, age), -1.0f, 1.0f);
        if (i == 0) nvgMoveTo(vg, px, py);
        else nvgLineTo(vg, px, py);
    }
    nvgLineJoin(vg, NVG_ROUND);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
    nvgRestore(vg);
}

void reductionMeter(NVGcontext* vg, const Theme& t, Rect r, float db, float range) {
    nvgSave(vg);
    well(vg, t, r, 2.0f);
    const Rect inner = r.inset(1.5f);
    const float depth = std::clamp(std::fabs(db) / std::max(1.0f, range), 0.0f, 1.0f);
    if (depth > 0.0f) {
        nvgBeginPath(vg);
        nvgRect(vg, inner.x, inner.y, inner.w, inner.h * depth);
        nvgFillPaint(vg, nvgLinearGradient(vg, 0, inner.y, 0, inner.bottom(), rgb(t.accent), rgba(t.accent, 0.55f)));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

}  // namespace supple::gui::widgets
