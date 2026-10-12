// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's editor: layout and the voice display. The envelope editor,
// the on-screen keyboard, gestures, text entry, meters and repaint
// decisions come from EditorBase.
#pragma once

#include "gui/EditorBase.h"
#include "Processor.h"

namespace product {

inline constexpr float kEditorWidth = 720.0f;
inline constexpr float kEditorHeight = 510.0f;

class Editor : public supple::gui::EditorBase {
  public:
    explicit Editor(supple::gui::EditorHost& host);
    void paint(NVGcontext* vg) override;
    void onTelemetry(const supple::TelemetryFrame& frame) override;

  private:
    int16_t keys_[Processor::kVoices];
};

}  // namespace product
