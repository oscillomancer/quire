// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The embedded native view: pugl owns the window, the graphics context and
// the event stream; this adapts them to an EditorBase. Nothing here runs a
// loop or a thread -- the host drives everything through tick() or, where a
// host offers no timer, through a view-owned fallback timer.
//
// Two units meet here. The host's: logical points on macOS (CLAP's Cocoa
// sizes), physical pixels on X11 and Windows. Pugl's: backing pixels on every
// platform, for sizes, configure events and the pointer alike. The public
// API speaks host units; inside, everything is pugl pixels, and the editor
// draws in its own design units through the one transform in draw().
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <nanovg.h>

#include "gui/NativeFiles.h"
#include "gui/Ui.h"

struct PuglViewImpl;
struct PuglWorldImpl;

namespace supple::gui {

class EditorBase;

// Also the editor's clipboard, through pugl's: plain text only. It gives
// the editor the system's file dialogs (NativeFiles), and files dropped on
// it (pugl's text/uri-list, every platform).
class Window : public Clipboard {
  public:
    // standalone selects pugl's world type. Inside a host this must stay
    // false: the host owns the application and pumps the native event loop,
    // and on macOS a PUGL_PROGRAM world would try to pump it from under it.
    Window(EditorBase& editor, const char* className, bool standalone = false);
    ~Window() override;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Allocates the view. The native window is created in embed(), because
    // pugl cannot reparent after realizing.
    bool create();
    void destroy();
    bool embed(uintptr_t parent);
    bool show();
    bool hide();

    // In host units.
    uint32_t width() const { return hostW_; }
    uint32_t height() const { return hostH_; }
    // False, changing nothing, for a size the view cannot take.
    bool setSize(uint32_t w, uint32_t h);
    // Bounds the user may resize to, in host units.
    void setSizeLimits(uint32_t minW, uint32_t minH, uint32_t maxW, uint32_t maxH);

    // Host timer tick: pumps pending events, idles the editor, repaints if
    // anything changed. Never blocks.
    void tick();
    bool startFallbackTimer(double seconds);
    void stopFallbackTimer();
    bool realized() const { return view_ && realized_; }
    // A point in design units, in host units from the window's top left: where
    // a host shows a menu the editor asks for.
    void designToHost(float x, float y, int32_t& hostX, int32_t& hostY) const;

    // Called from the pugl event thunk; not part of the API.
    void handleEvent(const void* event);

    // The most pixels a view spans (PuglSpan is 16 bits), and host units at
    // a backing scale in whole pixels: past that span kMaxSpan + 1, never a
    // size that wrapped round to a small one.
    static constexpr uint32_t kMaxSpan = 0xFFFF;
    static uint32_t pixels(uint32_t hostUnits, double scale);

    // Clipboard
    bool copy(std::string_view text) override;
    bool paste(std::function<void(std::string_view)> done) override;

    // Invoked at the end of each draw with the context current. Only the
    // offline preview tool sets it.
    void (*onFrameDrawn)(void* user) = nullptr;
    void* frameUser = nullptr;

  private:
    void frame();  // idle the editor and request a repaint if needed
    void draw();
    void syncCursor();
    struct Fit {
        float scale, offsetX, offsetY;
    };
    Fit fit() const;
    float toDesignX(double x) const;
    float toDesignY(double y) const;

    EditorBase& editor_;
    const char* className_;
    bool standalone_;
    PuglWorldImpl* world_ = nullptr;
    PuglViewImpl* view_ = nullptr;
    NVGcontext* vg_ = nullptr;
    // Pugl pixels per host unit: the backing scale on macOS, 1 elsewhere.
    double pixelsPerUnit() const;
    uint32_t toPixels(uint32_t hostUnits) const;
    uint32_t hostW_, hostH_;   // host units
    uint32_t width_, height_;  // pugl pixels
    uint32_t minW_ = 0, minH_ = 0, maxW_ = 0, maxH_ = 0;
    bool realized_ = false;
    bool fallbackTimer_ = false;
    std::chrono::steady_clock::time_point lastIdle_{};
    bool haveIdle_ = false;
    double lastPressTime_ = -1.0;
    float lastPressX_ = 0.0f, lastPressY_ = 0.0f;
    int lastPressButton_ = -1;
    int cursor_ = 0;
    std::function<void(std::string_view)> paste_;  // waiting for the clipboard's text
    std::unique_ptr<NativeFiles> files_;
};

}  // namespace supple::gui
