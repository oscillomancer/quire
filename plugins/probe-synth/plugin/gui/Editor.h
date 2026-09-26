// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#pragma once

#include <cstdint>

#include <nanovg.h>

#include "../src/ProbeSynthParams.h"
#include "Telemetry.h"
#include "Theme.h"

namespace probesynth::gui {

enum class EditorKey { Tab, Left, Right, Up, Down, Home, End, Enter, Space };

class Editor {
  public:
    explicit Editor(EditorHost& host) : host_(host) {
        for (int i = 0; i < kHistoryColumns; ++i) historyIn_[i] = historyOut_[i] = -120.0f;
    }
    bool attach(NVGcontext* vg);
    void onContextLost() { fontsReady_ = false; }
    void pump(double elapsedSeconds);
    void render(NVGcontext* vg);
    bool wantsRepaint() const;
    void markDirty() { dirty_ = true; }

    bool onMouseDown(float x, float y);
    void onMouseMove(float x, float y, bool fine);
    void onMouseUp();
    void onPointerLeave();
    void onDoubleClick(float x, float y);
    bool onScroll(float x, float y, float dy);
    void onKey(EditorKey key, bool fineOrReverse);
    void onFocusLost();
    bool dragging() const { return dragId_ >= 0; }

  private:
    int controlAt(float x, float y) const;
    void setFromNormal(uint32_t id, double normal);
    void focusParam(uint32_t id);
    void closeScrollGesture();
    void drawFocus(NVGcontext* vg);

    EditorHost& host_;
    float historyIn_[kHistoryColumns]{};
    float historyOut_[kHistoryColumns]{};
    int historyHead_ = 0;
    float historyFill_ = 0.0f;
    float latestIn_ = -120.0f, latestOut_ = -120.0f;
    float heldIn_ = -120.0f, heldOut_ = -120.0f;
    float holdAgeIn_ = 0.0f, holdAgeOut_ = 0.0f;
    int dragId_ = -1, focus_ = -1, scrollId_ = -1, hover_ = -1;
    float dragStartY_ = 0.0f, scrollIdle_ = 0.0f;
    double dragStartNormal_ = 0.0;
    bool fontsReady_ = false;
    mutable bool dirty_ = true;
    double shown_[kNumParams]{};
};

}  // namespace probesynth::gui
