// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Analyzer GUI — the object a plugin format holds on to.
//
// Owns the editor and its window and nothing else. The format layer implements
// EditorHost (parameter access and telemetry) and drives tick() from whatever
// timer it has; everything CLAP-specific stays on that side of the line.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Editor.h"
#include "Theme.h"
#include "Window.h"

namespace analyzer::gui {

class Gui {
  public:
    explicit Gui(EditorHost& host, bool standalone = false)
        : editor_(host), window_(editor_, standalone) {}
    ~Gui() { editor_.onFocusLost(); }

    bool create() { return window_.create(); }
    void destroy() { editor_.onFocusLost(); window_.destroy(); }

    bool setParent(uintptr_t parent) { return window_.embed(parent); }
    bool show() { return window_.show(); }
    bool hide() { return window_.hide(); }

    void getSize(uint32_t& w, uint32_t& h) const { window_.size(w, h); }
    bool setSize(uint32_t w, uint32_t h) { return window_.setSize(w, h); }

    // The panel is a fixed composition, so the only freedom is scale. Snap the
    // host's proposed size to the nearest size that preserves the aspect and
    // fits inside what it offered -- fitting inside matters, because a host
    // that gets back something larger than it asked for will clip.
    bool adjustSize(uint32_t& w, uint32_t& h) const {
        double scale = std::min((double)w / kDesignW, (double)h / kDesignH);
        scale = std::max(0.6, std::min(2.0, scale));
        w = (uint32_t)std::lround(kDesignW * scale);
        h = (uint32_t)std::lround(kDesignH * scale);
        return true;
    }

    void setScale(double s) { window_.setScale(s); }

    void tick() { window_.tick(); }

    bool startFallbackTimer(double seconds) { return window_.startFallbackTimer(seconds); }
    void stopFallbackTimer() { window_.stopFallbackTimer(); }

    // Only the offline preview tool uses this; see Window::onFrameDrawn.
    void setFrameCallback(void (*fn)(void*), void* user) {
        window_.onFrameDrawn = fn;
        window_.frameUser = user;
    }

    // Also only the preview tool: it drives a click so that transient panel
    // states -- an open menu, a held control -- can be reviewed offline
    // instead of only ever capturing the panel at rest.
    void previewClick(float x, float y) { editor_.onMouseDown(x, y); }
    void previewMove(float x, float y) { editor_.onMouseMove(x, y, false); }
    void previewRepaint() { editor_.markDirty(); }

    static void resizeHints(bool& horizontal, bool& vertical, bool& preserveAspect,
                            uint32_t& aw, uint32_t& ah) {
        horizontal = vertical = preserveAspect = true;
        aw = (uint32_t)kDesignW;
        ah = (uint32_t)kDesignH;
    }

  private:
    Editor editor_;
    Window window_;
};

}  // namespace analyzer::gui
