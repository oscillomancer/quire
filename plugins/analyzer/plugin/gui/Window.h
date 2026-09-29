// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Analyzer GUI — the embedded window.
//
// pugl owns the native view, the GL context and the event stream; this wraps it
// in the shape clap_plugin_gui wants. Nothing here runs a loop or a thread: the
// host drives us through tick(), which is what a plugin is supposed to do.
#pragma once

#include <chrono>
#include <cstdint>

#include "Theme.h"

struct PuglViewImpl;
struct PuglWorldImpl;

namespace analyzer::gui {

class Editor;

// --- Extended input ---------------------------------------------------------
//
// The basic editor vocabulary -- onMouseDown(x, y), onMouseMove(x, y, fine),
// onMouseUp(), onDoubleClick(x, y), onScroll(x, y, dy), onKey(EditorKey, bool)
// and onFocusLost() -- is enough for a panel of knobs. An editor that is a
// direct-manipulation surface needs the rest of what pugl reports: which
// button, which modifiers, wheel deltas on both axes, hover without a button
// held, the full key set, typed text, and a pointer cursor that says what a
// drag would do. Rather than fork Window.cpp per product, all of that arrives
// through an OPTIONAL extended API that Window detects at compile time (the
// dispatch helpers in Window.cpp). An editor that declares none of it gets
// exactly the basic calls it always did, which is what keeps this file
// byte-identical across products.
//
// The extended methods, each optional and independent of the others:
//   bool onPointerDown(float x, float y, int button, uint32_t mods);
//   void onPointerDouble(float x, float y, int button, uint32_t mods);
//   void onPointerUp(float x, float y, int button, uint32_t mods);
//   void onPointerMove(float x, float y, uint32_t mods);   // hover and drag alike
//   bool onWheel(float x, float y, float dx, float dy, uint32_t mods);
//   bool onKeyCode(uint32_t key, uint32_t mods);  // true = handled, else EditorKey path
//   void onKeyRelease(uint32_t key, uint32_t mods);
//   void onText(uint32_t codepoint);
//   void onPointerEnter();
//   void onPointerLeave();                        // replaces the basic drag-ending rule
//   Cursor cursor() const;                        // polled after every pointer event
//
// When onPointerDown exists it receives every button and the basic
// onMouseDown/onDoubleClick are not called; likewise onPointerUp replaces
// onMouseUp, onPointerMove replaces onMouseMove and onWheel replaces onScroll.
// Buttons are 0 left, 1 right, 2 middle. Keys are pugl's: the Unicode code
// point for a printable key, unshifted, and the constants below for the rest.

enum Mod : uint32_t { kModShift = 1u, kModCtrl = 2u, kModAlt = 4u, kModSuper = 8u };

enum class Cursor { Arrow, Hand, Crosshair, Caret, LeftRight, UpDown, AllScroll, No };

namespace key {
constexpr uint32_t kBackspace = 0x0008u, kTab = 0x0009u, kEnter = 0x000Du, kEscape = 0x001Bu,
                   kDelete = 0x007Fu, kSpace = 0x0020u, kF1 = 0xE000u, kPageUp = 0xE031u,
                   kPageDown = 0xE032u, kEnd = 0xE033u, kHome = 0xE034u, kLeft = 0xE035u,
                   kUp = 0xE036u, kRight = 0xE037u, kDown = 0xE038u, kPadEnter = 0xE070u;
}

class Window {
  public:
    // standalone selects pugl's world type. Inside a host this must stay false:
    // the host owns the application object and pumps the native event loop, and
    // on macOS a PUGL_PROGRAM world would try to pump it from under the host.
    // A standalone executable has no such owner and must pump for itself.
    explicit Window(Editor& editor, bool standalone = false)
        : editor_(editor), standalone_(standalone) {}
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Allocates the view and applies hints. The native window is not created
    // until embed(), because pugl cannot reparent after realization.
    bool create();
    void destroy();

    bool embed(uintptr_t parent);
    bool show();
    bool hide();

    void size(uint32_t& w, uint32_t& h) const { w = width_; h = height_; }
    bool setSize(uint32_t w, uint32_t h);
    void setScale(double s);

    // Called from the host's timer. Processes pending events and asks for a
    // repaint; never blocks.
    void tick();

    // For hosts that do not implement clap.timer-support, drive repaints from
    // a view-owned timer instead. Only meaningful once realized.
    bool startFallbackTimer(double seconds);
    void stopFallbackTimer();
    bool fallbackTimerRunning() const { return fallbackTimer_; }

    bool realized() const { return view_ != nullptr && realized_; }

    // Called by the pugl event thunk in Window.cpp; not part of the API.
    void handleEvent(const void* event);

    // Invoked at the end of each draw, with the GL context still current.
    // Only the offline preview tool sets this; it is null in the plugin.
    void (*onFrameDrawn)(void* user) = nullptr;
    void* frameUser = nullptr;

  private:
    void draw();
    void syncCursor();

    // The panel is drawn in design units and scaled once. Aspect is pinned, so
    // these agree, but both are computed rather than assumed.
    double scaleX() const { return (double)width_ / (double)kDesignW; }
    double scaleY() const { return (double)height_ / (double)kDesignH; }

    Editor& editor_;
    PuglWorldImpl* world_ = nullptr;
    PuglViewImpl* view_ = nullptr;
    NVGcontext* vg_ = nullptr;

    // From the theme, so the window and the layout cannot disagree.
    uint32_t width_ = (uint32_t)kDesignW;
    uint32_t height_ = (uint32_t)kDesignH;
    bool realized_ = false;
    bool standalone_ = false;
    bool fallbackTimer_ = false;
    std::chrono::steady_clock::time_point lastDrawTime_{};
    bool haveDrawTime_ = false;

    // Double-click detection: pugl reports presses, not clicks.
    double lastClickTime_ = 0.0;
    float lastClickX_ = 0.0f, lastClickY_ = 0.0f;

    // The cursor last handed to pugl, so an editor that reports the same
    // shape on every motion event does not cost a platform call each time.
    Cursor cursor_ = Cursor::Arrow;
};

}  // namespace analyzer::gui
