// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: the impulse response's file slot, and four knobs.
// Choosing, dropping, clearing and "missing" all come from Ui::fileSlot.
#pragma once

#include "gui/EditorBase.h"

namespace product {

inline constexpr float kEditorWidth = 560.0f;
inline constexpr float kEditorHeight = 352.0f;

class Editor : public supple::gui::EditorBase {
  public:
    explicit Editor(supple::gui::EditorHost& host);
    void paint(NVGcontext* vg) override;
};

}  // namespace product
