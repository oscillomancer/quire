// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The automatic editor: what a product with no editor code gets
// (PRODUCT_EDITOR_SOURCES left empty in product.cmake). Every parameter,
// laid out by group: a panel per group (.in("Filter")), in table order,
// each control chosen from the parameter -- a knob for a value, a toggle, a
// segmented switch for up to four choices and a dropdown for more, a
// readout for an output -- and the bypass, the presets and the meters in
// the header. Properties get text fields, file properties file slots. Help
// text is each tooltip.
//
// It is a start, not a design: a product past its first sketch writes its
// own Editor.cpp and lists it in product.cmake, which drops this file.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/Definition.h"
#include "gui/EditorBase.h"
#include "gui/Widgets.h"

// The product's palette, if it has one: angle brackets, so the search is the
// include path (where product/ is) and not this directory's own Theme.h.
#if __has_include(<Theme.h>)
#include <Theme.h>
#define SUPPLE_PRODUCT_THEME 1
#endif

namespace supple::gui {
namespace {

#if SUPPLE_PRODUCT_THEME
const Theme& autoTheme() { return product::kTheme; }
#else
// The template's palette, for a product without Theme.h.
const Theme& autoTheme() {
    static const Theme theme = {0x0B0D11, 0x161A21, 0x20252E, 0x07090C, 0x252C37, 0x343D4A, 0x5D6674, 0x8F98A5,
                                0xE8E4DC, 0xE8944A, 0x6FB3A8, 0x9D8DF1, 0xE0584A, 0x0D1015, 0x3B4553, 0x181D25,
                                0x48535F, 7.0f};
    return theme;
}
#endif

constexpr float kMargin = 24.0f, kGap = 16.0f, kHeader = 60.0f, kPad = 16.0f, kTitle = 28.0f;
constexpr float kMaxInner = 436.0f;  // four knobs across, then the next row
constexpr float kMaxWidth = 1000.0f;
constexpr float kCaption = 16.0f;    // a caption above a switch, menu or field

struct Cell {
    Widget widget = Widget::Knob;
    uint32_t id = 0;      // a parameter's, or a property's for a text field
    float w = 0.0f, h = 0.0f;
    bool caption = false;  // drawn above it, from its name
    Rect at;               // where it went, the caption included
};

struct Group {
    std::string title;
    std::vector<Cell> cells;
    bool meters = false;
    Rect panel;
};

Cell cellFor(const ParamDef& p) {
    Cell c;
    c.id = p.id;
    if (p.isOutput()) {
        c.widget = Widget::Readout;
        c.w = 160.0f;
        c.h = 34.0f;
    } else if (p.kind == ParamKind::Toggle) {
        c.widget = Widget::Toggle;
        c.w = 120.0f;
        c.h = 30.0f;
    } else if (p.kind == ParamKind::Choice) {
        const int options = static_cast<int>(p.hostMax() - p.hostMin()) + 1;
        c.caption = true;
        if (options <= 4) {
            c.widget = Widget::Segmented;
            c.w = std::clamp(62.0f * static_cast<float>(options), 140.0f, 300.0f);
        } else {
            c.widget = Widget::Dropdown;
            c.w = 160.0f;
        }
        c.h = 28.0f + kCaption;
    } else {
        c.widget = Widget::Knob;
        c.w = 92.0f;
        c.h = 100.0f;
    }
    return c;
}

// Each group's cells placed in rows inside it; the panels placed in rows
// across the editor. Returns the editor's size.
struct Plan {
    std::vector<Group> groups;
    float width = 0.0f, height = 0.0f;
};

Plan plan(const Definition& definition) {
    const ParamTable& params = definition.params();
    const PropertyList& properties = definition.properties();
    Plan out;
    auto groupNamed = [&](const std::string& title) -> Group& {
        for (Group& g : out.groups)
            if (g.title == title && !g.meters) return g;
        out.groups.push_back({title, {}, false, {}});
        return out.groups.back();
    };
    for (size_t i = 0; i < params.count(); ++i) {
        const ParamDef& p = params.at(i);
        if (p.is(pflag::kHidden) || p.is(pflag::kBypass)) continue;
        groupNamed(p.group ? p.group : "").cells.push_back(cellFor(p));
    }
    for (size_t i = 0; i < properties.count(); ++i) {
        Cell c;
        const bool file = properties.at(i).isFile();
        c.widget = file ? Widget::File : Widget::TextField;
        c.id = properties.at(i).id;
        c.w = file ? 300.0f : 200.0f;
        c.h = 26.0f + kCaption;
        c.caption = true;
        groupNamed("").cells.push_back(c);
    }
    if (definition.info().audioOutput()) out.groups.push_back({"Level", {}, true, {}});

    // Inside each panel: rows of cells, as wide as kMaxInner allows.
    for (Group& g : out.groups) {
        float x = 0.0f, y = 0.0f, rowHeight = 0.0f, widest = 0.0f;
        for (Cell& c : g.cells) {
            if (x > 0.0f && x + c.w > kMaxInner) {
                x = 0.0f;
                y += rowHeight + kGap;
                rowHeight = 0.0f;
            }
            c.at = {x, y, c.w, c.h};
            x += c.w + kGap;
            rowHeight = std::max(rowHeight, c.h);
            widest = std::max(widest, x - kGap);
        }
        // Each row's cells centred on the row's height.
        for (Cell& c : g.cells) {
            float tallest = 0.0f;
            for (const Cell& other : g.cells)
                if (other.at.y == c.at.y) tallest = std::max(tallest, other.h);
            c.at.y += (tallest - c.h) * 0.5f;
        }
        const float top = g.title.empty() ? kPad : kPad + kTitle;
        if (g.meters) {  // two bars side by side; its height comes later
            widest = 92.0f;
            y = 0.0f;
            rowHeight = 0.0f;
        }
        g.panel = {0.0f, 0.0f, std::max(widest, 120.0f) + 2.0f * kPad, top + y + rowHeight + kPad};
    }

    // Panels across the editor, wrapping; a row's panels share its height.
    // The meters, if any, stand at the right, as tall as everything else.
    Group* meters = !out.groups.empty() && out.groups.back().meters ? &out.groups.back() : nullptr;
    const size_t panels = out.groups.size() - (meters ? 1 : 0);
    const float limit = kMaxWidth - kMargin - (meters ? meters->panel.w + kGap : 0.0f);
    float x = kMargin, y = kHeader + kGap, rowHeight = 0.0f, widest = kMargin;
    size_t rowStart = 0;
    auto endRow = [&](size_t end) {
        for (size_t k = rowStart; k < end; ++k) out.groups[k].panel.h = rowHeight;
    };
    for (size_t k = 0; k < panels; ++k) {
        Group& g = out.groups[k];
        if (x > kMargin && x + g.panel.w > limit) {
            endRow(k);
            rowStart = k;
            x = kMargin;
            y += rowHeight + kGap;
            rowHeight = 0.0f;
        }
        g.panel.x = x;
        g.panel.y = y;
        x += g.panel.w + kGap;
        rowHeight = std::max(rowHeight, g.panel.h);
        widest = std::max(widest, x - kGap);
    }
    endRow(panels);
    float bottom = y + rowHeight;
    if (meters) {
        meters->panel.x = panels > 0 ? widest + kGap : kMargin;
        meters->panel.y = kHeader + kGap;
        meters->panel.h = std::max(bottom - meters->panel.y, 220.0f);
        widest = meters->panel.right();
        bottom = std::max(bottom, meters->panel.bottom());
    }
    const float header = (definition.info().presets ? 296.0f + 24.0f : 0.0f) + 112.0f + 300.0f;
    out.width = std::max({widest + kMargin, header, 480.0f});
    out.height = bottom + kMargin;
    return out;
}

class AutoEditor : public EditorBase {
  public:
    AutoEditor(EditorHost& host, Plan layout)
        : EditorBase(host, autoTheme(), layout.width, layout.height), plan_(std::move(layout)) {
        float right = width() - kMargin;
        const ParamTable& params = paramTable();
        if (params.bypassIndex() >= 0) {
            ui().toggle(params.at(static_cast<size_t>(params.bypassIndex())).id, {right - 112.0f, 16.0f, 112.0f, 28.0f});
            right -= 112.0f + 16.0f;
        }
        if (definition().info().presets) ui().presetBar({right - 296.0f, 16.0f, 296.0f, 28.0f});
        for (Group& g : plan_.groups) {
            const Rect body = inner(g);
            for (Cell& c : g.cells) {
                Rect r = c.at.offset(body.x, body.y);
                if (c.caption) r.cutTop(kCaption);
                Control& control = ui().add(c.id, c.widget, r);
                if (c.widget == Widget::TextField) control.label = propertyName(c.id);
                // Dimmed while the definition says it does not apply.
                const bool property = c.widget == Widget::TextField || c.widget == Widget::File;
                const int index = property ? -1 : params.indexOf(c.id);
                if (index >= 0)
                    control.enabledWhen([this, index] {
                        const ParamTable& table = paramTable();
                        return definition().applies(static_cast<size_t>(index), [&](size_t k) {
                            return this->host().value(table.at(k).id);
                        });
                    });
            }
        }
        usePeakMeters(definition().info().audioOutput());
    }

    void paint(NVGcontext* vg) override {
        const Theme& t = theme();
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, width(), height());
        nvgFillColor(vg, rgb(t.ground));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, width(), kHeader);
        nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, kHeader, rgb(t.surface), rgb(t.ground)));
        nvgFill(vg);
        widgets::hairline(vg, 0, kHeader - 0.5f, width(), kHeader - 0.5f, rgb(t.line));
        std::string name = definition().info().name;
        for (char& ch : name) ch = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
        nvgSave(vg);
        nvgFontFace(vg, font::kDisplay);
        nvgFontSize(vg, 25.0f);
        nvgTextLetterSpacing(vg, 2.5f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, rgb(t.text));
        nvgText(vg, kMargin + 2.0f, 27.0f, name.c_str(), nullptr);
        nvgRestore(vg);
        char version[96];
        std::snprintf(version, sizeof(version), "%s  \xC2\xB7  v%s", definition().info().vendor.c_str(),
                      definition().info().version.c_str());
        widgets::caption(vg, kMargin + 3.0f, 47.0f, version, rgb(t.textFaint), widgets::Align::Left, 9.0f);

        for (const Group& g : plan_.groups) {
            widgets::panel(vg, t, g.panel, g.title.empty() ? nullptr : g.title.c_str());
            const Rect body = inner(g);
            if (g.meters) {
                const Rect m{body.x + 4.0f, body.y, 34.0f, body.h - 18.0f};
                const Rect o{m.right() + 12.0f, body.y, 34.0f, body.h - 18.0f};
                widgets::meter(vg, t, m, inputMeters(), meterChannels());
                widgets::meter(vg, t, o, outputMeters(), meterChannels());
                widgets::caption(vg, m.cx(), m.bottom() + 12.0f, "In", rgb(t.textMuted), widgets::Align::Center, 9.0f);
                widgets::caption(vg, o.cx(), o.bottom() + 12.0f, "Out", rgb(t.textMuted), widgets::Align::Center, 9.0f);
                continue;
            }
            for (const Cell& c : g.cells) {
                if (!c.caption) continue;
                const Rect r = c.at.offset(body.x, body.y);
                const char* text = c.widget == Widget::TextField || c.widget == Widget::File
                                       ? propertyName(c.id)
                                       : paramTable().find(c.id)->displayLabel();
                widgets::caption(vg, r.x + 1.0f, r.y + 6.0f, text, rgb(t.textMuted), widgets::Align::Left, 9.5f);
            }
        }
        drawControls(vg);
    }

  private:
    const char* propertyName(uint32_t id) const {
        const PropertyList& properties = definition().properties();
        const int k = properties.indexOf(id);
        return k >= 0 ? properties.at(static_cast<size_t>(k)).name : "";
    }

    static Rect inner(const Group& g) {
        Rect body = g.panel.inset(kPad);
        if (!g.title.empty()) body.cutTop(kTitle);
        return body;
    }

    Plan plan_;
};

}  // namespace
}  // namespace supple::gui

namespace product {
std::unique_ptr<supple::gui::EditorBase> createEditor(supple::gui::EditorHost& host) {
    return std::make_unique<supple::gui::AutoEditor>(host, supple::gui::plan(host.pluginDefinition()));
}
}  // namespace product
