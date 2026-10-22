// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The automatic editor (AutoEditor.cpp), with panels of a layer's own after
// the parameters' panels: state that is not a parameter, which the layer
// knows how to show and edit -- a script's modulation routes, say. A
// layer's createEditor() (plugin/Layer.h) makes it with automaticEditor();
// product::createEditor() is the same editor with none. It exists only in
// a product with no editor code of its own.
//
// A panel draws its body and takes the pointer there itself, with the
// rules EditorBase's own hooks have: a press it takes holds the pointer
// until that button is released, and a menu or text field Ui has open
// comes first. Ui's popup() and prompt() give it the editor's menus and
// text fields. Main thread only.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "gui/Layout.h"
#include "gui/Ui.h"

struct NVGcontext;

namespace supple::gui {

class EditorBase;
class EditorHost;
struct Theme;

class AutoPanel {
  public:
    virtual ~AutoPanel() = default;

    // The panel's title, and the room its body takes, in design units: as
    // wide as asked, and at least as tall (panels in a row share a height).
    virtual const char* title() const = 0;
    virtual float width() const = 0;
    virtual float height() const = 0;

    // Once, as the editor opens: where its body went, and the editor's host
    // and Ui, which outlive the panel. A press it takes may open a menu
    // (Ui::popup()) or a text field (Ui::prompt()).
    virtual void open(EditorHost& host, Ui& ui, Rect body) = 0;
    // Each idle tick: true when what it shows has changed and the editor
    // should repaint (a property the host changed, say).
    virtual bool idle() { return false; }
    // Its body, under the editor's menus and text fields.
    virtual void paint(NVGcontext* vg, const Theme& theme) = 0;

    // The pointer, in design units: true to take the event. A press comes
    // only within its body; moves and the wheel come there too, and moves
    // and the release wherever they are while it holds a press.
    virtual bool pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
        (void)x, (void)y, (void)button, (void)mods, (void)doubleClick;
        return false;
    }
    virtual bool pointerMove(float x, float y, uint32_t mods) {
        (void)x, (void)y, (void)mods;
        return false;
    }
    virtual void pointerUp(float x, float y, int button, uint32_t mods) {
        (void)x, (void)y, (void)button, (void)mods;
    }
    virtual bool wheel(float x, float y, float dx, float dy, uint32_t mods) {
        (void)x, (void)y, (void)dx, (void)dy, (void)mods;
        return false;
    }
    // The pointer left its body, or the editor; drop any hover.
    virtual void pointerLeave() {}
    // Focus was lost or the editor is closing: drop whatever a press began.
    virtual void cancel() {}
    // The cursor over its body at (x, y).
    virtual Cursor cursor(float x, float y) const {
        (void)x, (void)y;
        return Cursor::Arrow;
    }
};

std::unique_ptr<EditorBase> automaticEditor(EditorHost& host,
                                            std::vector<std::unique_ptr<AutoPanel>> panels = {});

}  // namespace supple::gui
