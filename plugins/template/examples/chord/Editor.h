// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: layout, and a keyboard showing the chord shape and
// the last chord played. Controls, gestures, keyboard, text entry, the
// preset bar and repaint decisions come from EditorBase.
#pragma once

#include <cstdint>

#include "gui/EditorBase.h"
#include "Processor.h"

namespace product {

inline constexpr float kEditorWidth = 640.0f;
inline constexpr float kEditorHeight = 350.0f;

class Editor : public supple::gui::EditorBase {
  public:
    explicit Editor(supple::gui::EditorHost& host);
    void paint(NVGcontext* vg) override;
    void onTelemetry(const supple::TelemetryFrame& frame) override;

  private:
    void drawKeys(NVGcontext* vg, supple::gui::Rect r);

    int16_t last_[Processor::kMaxNotes] = {-1, -1, -1, -1, -1, -1};
};

}  // namespace product
