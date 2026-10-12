// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: two pages, the oscillator's shape, an XY pad for
// depth and stereo phase, a preset browser, and a light and a dark look.
// Controls, gestures, keyboard, text entry, tooltips, meters and repaint
// decisions come from EditorBase.
#pragma once

#include "gui/EditorBase.h"

namespace product {

inline constexpr float kEditorWidth = 720.0f;
inline constexpr float kEditorHeight = 420.0f;

class Editor : public supple::gui::EditorBase {
  public:
    explicit Editor(supple::gui::EditorHost& host);
    void paint(NVGcontext* vg) override;

  protected:
    void drawControl(NVGcontext* vg, const supple::gui::Control& c, const supple::gui::ControlState& s) override;

  private:
    void drawShape(NVGcontext* vg, supple::gui::Rect r);

    int tabs_ = -1;
    bool light_ = false;
};

}  // namespace product
