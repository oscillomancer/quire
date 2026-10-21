// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The modulation routes of a plugin whose meaning has them, as a list to
// see and edit: a row a route -- its source, the parameter it moves, its
// amount in that parameter's unit, and the source it goes through -- and a
// button to add one. A panel of the automatic editor (gui/AutoEditor.h)
// for a script with routes and no editor of its own, and what a script's
// own editor places with e.routes() (script_editor.h).
//
//   - A source, a parameter or a via is a menu: the sources the plugin's
//     own first, then the instrument's; the parameters by group. What a
//     patch would refuse -- a route already there, a source with a value
//     per voice to a parameter all voices share -- is in it but cannot be
//     chosen.
//   - An amount is a bar from its middle, dragged across (Shift: finely),
//     turned by the wheel, typed after a double click ("+3 st", "-6 dB",
//     "1.5"), and reset to nothing by a double click with Ctrl or Cmd. A
//     right click on a row offers typing it, inverting it and removing the
//     route.
//   - Each change writes the routes property as a patch's "routes" member
//     would (PatchCodec::routeText), so the list makes only what a patch
//     can, in the same canonical text. A change made elsewhere (a patch, a
//     preset, undo) shows at the next idle tick; rows keep their order, and
//     a route new to the list goes at the end.
//
// Main thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Definition.h"
#include "gui/AutoEditor.h"
#include "supple/suite/semantics.h"

namespace supple::suite {

class RouteList final : public gui::AutoPanel {
  public:
    // The definition (a SemanticDefinition, or any whose meaning has routes)
    // outlives the list.
    RouteList(const Definition& definition, float width, float height);

    // Whether a definition has routes to show.
    static bool shows(const Definition& definition);

    const char* title() const override { return "Modulation"; }
    float width() const override { return width_; }
    float height() const override { return height_; }
    void open(gui::EditorHost& host, gui::Ui& ui, gui::Rect body) override;
    bool idle() override;
    void paint(NVGcontext* vg, const gui::Theme& theme) override;
    bool pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) override;
    bool pointerMove(float x, float y, uint32_t mods) override;
    void pointerUp(float x, float y, int button, uint32_t mods) override;
    bool wheel(float x, float y, float dx, float dy, uint32_t mods) override;
    void pointerLeave() override;
    void cancel() override;
    gui::Cursor cursor(float x, float y) const override;

    // What the list shows, for tests: a row a route.
    struct Row {
        std::string source, destination, via;  // keys; via empty: none
        double amount = 0.0;
    };
    const std::vector<Row>& rows() const { return rows_; }
    // Where it is in the editor, once open.
    gui::Rect bounds() const { return body_; }

  private:
    enum class Part { None, Source, Destination, Amount, Via, Remove, Add, Scroll };
    struct Hit {
        Part part = Part::None;
        int row = -1;
    };

    // Where things are.
    gui::Rect rowRect(int row) const;  // by row index, scrolled
    gui::Rect partRect(int row, Part part) const;
    gui::Rect addRect() const;
    int visibleRows() const;
    Hit hit(float x, float y) const;

    // The routes as the host has them, into rows (keeping their order).
    void read();
    // The rows as the routes property; false, changing nothing, when a patch
    // would refuse them.
    bool write(std::vector<Row> rows);

    // Menus.
    void chooseSource(int row, gui::Rect anchor);
    void chooseDestination(int row, gui::Rect anchor, std::string source);
    void chooseVia(int row, gui::Rect anchor);
    void rowMenu(int row, float x, float y);
    void typeAmount(int row);

    // What a parameter and a source are, by key.
    int paramIndex(const std::string& key) const;
    const SourceMeaning* source(const std::string& key) const;
    bool builtIn(const std::string& key) const;
    // Whether a route from `source` (through `via`) to parameter `index` is
    // one a patch takes, ignoring row `except` for the one-per-pair rule.
    bool allowed(const std::string& source, const std::string& via, size_t index, int except) const;
    double reach(size_t index) const;
    const char* unit(size_t index) const;
    int decimals(size_t index) const;
    double rounded(size_t index, double amount) const;
    std::string amountText(size_t index, double amount) const;
    std::string shortName(size_t index) const;  // its name, without its group's
    std::string sourceName(const std::string& key) const;

    const Definition& definition_;
    PatchCodec codec_;
    float width_, height_;
    gui::EditorHost* host_ = nullptr;
    gui::Ui* ui_ = nullptr;
    gui::Rect body_;
    uint32_t property_ = 0;  // the routes property's ID
    uint64_t version_ = ~uint64_t{0};
    std::string text_;  // what the property held when last read or written
    std::vector<Row> rows_;
    int scroll_ = 0;  // the first row shown
    Hit hover_;
    // A drag of an amount: its row, where it began and the amount then.
    int dragRow_ = -1;
    float dragX_ = 0.0f;
    double dragFrom_ = 0.0, dragAt_ = 0.0;
    bool fine_ = false;
};

}  // namespace supple::suite
