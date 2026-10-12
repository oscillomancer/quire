// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The object the plugin holds: the product's editor and its window, and the
// size arithmetic between them.
//
//   host size = design size x user scale x platform scale
//
// The user scale is what resizing changes; it is saved with the session.
// The platform scale is the host's clap_plugin_gui.set_scale() on Windows and
// X11, where sizes are physical pixels; macOS works in logical points and
// keeps it at 1.
#pragma once

#include <cstdint>
#include <memory>

#include "gui/EditorBase.h"
#include "gui/Window.h"

namespace supple::gui {

class Gui {
  public:
    static constexpr double kMinScale = 0.5;
    static constexpr double kMaxScale = 2.5;

    Gui(EditorHost& host, const char* className, bool standalone = false);
    ~Gui();
    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    bool create();
    bool setParent(uintptr_t parent) { return window_.embed(parent); }
    bool show() { return window_.show(); }
    bool hide() { return window_.hide(); }

    // In host units.
    void size(uint32_t& w, uint32_t& h) const;
    bool setSize(uint32_t w, uint32_t h);
    // Nearest size that keeps the aspect and fits inside what was offered.
    bool adjustSize(uint32_t& w, uint32_t& h) const;
    void setPlatformScale(double scale);
    double platformScale() const { return platformScale_; }
    double userScale() const { return userScale_; }
    void setUserScale(double scale);
    // The host size setUserScale(scale) would ask for, without asking.
    void sizeAt(double userScale, uint32_t& w, uint32_t& h) const;
    void aspect(uint32_t& w, uint32_t& h) const;

    void tick() { window_.tick(); }
    bool startFallbackTimer(double seconds) { return window_.startFallbackTimer(seconds); }
    void stopFallbackTimer() { window_.stopFallbackTimer(); }

    EditorBase& editor() { return *editor_; }
    Window& window() { return window_; }

  private:
    uint32_t hostW(double userScale) const;
    uint32_t hostH(double userScale) const;
    void applyLimits();

    std::unique_ptr<EditorBase> editor_;  // declared first: outlives the window
    Window window_;
    double platformScale_ = 1.0;
    double userScale_ = 1.0;
};

}  // namespace supple::gui
