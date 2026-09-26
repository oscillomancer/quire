// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// ProbeSynth GUI — the embedded window.
#include "Window.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

#include <pugl/gl.h>
#include <pugl/pugl.h>

#include <nanovg.h>

#include "Editor.h"
#include "Theme.h"

// Defined by whichever backend TU is compiled in -- NanoVGBackend.c for GL,
// MetalBackend.m for Metal. Declared rather than included so that no C++
// translation unit needs a GL or Metal header; see the note in NanoVGBackend.c.
// Nothing below this line names a renderer, which is the entire point.
extern "C" {
NVGcontext* probesynthNvgCreate(void* nativeContext);
void probesynthNvgDelete(NVGcontext* ctx);
int probesynthLoadGL(void);
void probesynthViewport(int w, int h);
void probesynthClear(NVGcontext* ctx, float r, float g, float b);
}

#if PROBESYNTH_METAL
#include "pugl_metal.h"
#endif

namespace probesynth::gui {
namespace {
// Any small id; pugl only requires that we recognise our own.
constexpr uintptr_t kFallbackTimerId = 1;

constexpr double kDoubleClickSeconds = 0.4;
constexpr float kDoubleClickSlop = 4.0f;

// --- extended input dispatch ------------------------------------------------
//
// Each optional editor method from Window.h is detected here, and the event
// handler below routes through these templates rather than calling the editor
// directly. The point of the template is the discarded branch: inside a
// function template an `if constexpr` branch that is not taken is never
// instantiated, so naming a method the product's Editor lacks is not an error.
// Written straight into handleEvent, which is not a template, both branches
// would have to compile and every product would need every method.

template <typename E, typename = void> struct HasPointerDown : std::false_type {};
template <typename E>
struct HasPointerDown<E, std::void_t<decltype(std::declval<E&>().onPointerDown(
                             0.0f, 0.0f, 0, uint32_t{}))>> : std::true_type {};

template <typename E, typename = void> struct HasPointerDouble : std::false_type {};
template <typename E>
struct HasPointerDouble<E, std::void_t<decltype(std::declval<E&>().onPointerDouble(
                               0.0f, 0.0f, 0, uint32_t{}))>> : std::true_type {};

template <typename E, typename = void> struct HasPointerUp : std::false_type {};
template <typename E>
struct HasPointerUp<E, std::void_t<decltype(std::declval<E&>().onPointerUp(
                           0.0f, 0.0f, 0, uint32_t{}))>> : std::true_type {};

template <typename E, typename = void> struct HasPointerMove : std::false_type {};
template <typename E>
struct HasPointerMove<E, std::void_t<decltype(std::declval<E&>().onPointerMove(
                             0.0f, 0.0f, uint32_t{}))>> : std::true_type {};

template <typename E, typename = void> struct HasWheel : std::false_type {};
template <typename E>
struct HasWheel<E, std::void_t<decltype(std::declval<E&>().onWheel(
                       0.0f, 0.0f, 0.0f, 0.0f, uint32_t{}))>> : std::true_type {};

template <typename E, typename = void> struct HasKeyCode : std::false_type {};
template <typename E>
struct HasKeyCode<E, std::void_t<decltype(std::declval<E&>().onKeyCode(uint32_t{}, uint32_t{}))>>
    : std::true_type {};

template <typename E, typename = void> struct HasKeyRelease : std::false_type {};
template <typename E>
struct HasKeyRelease<E, std::void_t<decltype(std::declval<E&>().onKeyRelease(uint32_t{},
                                                                             uint32_t{}))>>
    : std::true_type {};

template <typename E, typename = void> struct HasText : std::false_type {};
template <typename E>
struct HasText<E, std::void_t<decltype(std::declval<E&>().onText(uint32_t{}))>>
    : std::true_type {};

template <typename E, typename = void> struct HasPointerEnter : std::false_type {};
template <typename E>
struct HasPointerEnter<E, std::void_t<decltype(std::declval<E&>().onPointerEnter())>>
    : std::true_type {};

template <typename E, typename = void> struct HasPointerLeave : std::false_type {};
template <typename E>
struct HasPointerLeave<E, std::void_t<decltype(std::declval<E&>().onPointerLeave())>>
    : std::true_type {};

template <typename E, typename = void> struct HasCursor : std::false_type {};
template <typename E>
struct HasCursor<E, std::void_t<decltype(std::declval<const E&>().cursor())>>
    : std::true_type {};

uint32_t modsOf(PuglMods state) {
    uint32_t m = 0;
    if (state & PUGL_MOD_SHIFT) m |= kModShift;
    if (state & PUGL_MOD_CTRL) m |= kModCtrl;
    if (state & PUGL_MOD_ALT) m |= kModAlt;
    if (state & PUGL_MOD_SUPER) m |= kModSuper;
    return m;
}

PuglCursor puglCursorOf(Cursor c) {
    switch (c) {
        case Cursor::Hand: return PUGL_CURSOR_HAND;
        case Cursor::Crosshair: return PUGL_CURSOR_CROSSHAIR;
        case Cursor::Caret: return PUGL_CURSOR_CARET;
        case Cursor::LeftRight: return PUGL_CURSOR_LEFT_RIGHT;
        case Cursor::UpDown: return PUGL_CURSOR_UP_DOWN;
        case Cursor::AllScroll: return PUGL_CURSOR_ALL_SCROLL;
        case Cursor::No: return PUGL_CURSOR_NO;
        default: return PUGL_CURSOR_ARROW;
    }
}

template <typename E>
void dispatchPress(E& ed, float x, float y, int button, uint32_t mods, bool dbl) {
    if constexpr (HasPointerDown<E>::value) {
        if (dbl) {
            if constexpr (HasPointerDouble<E>::value) {
                ed.onPointerDouble(x, y, button, mods);
                return;
            }
        }
        ed.onPointerDown(x, y, button, mods);
    } else {
        (void)mods;
        if (button != 0) return;
        if (dbl) ed.onDoubleClick(x, y);
        else ed.onMouseDown(x, y);
    }
}

template <typename E>
void dispatchRelease(E& ed, float x, float y, int button, uint32_t mods) {
    if constexpr (HasPointerUp<E>::value) {
        ed.onPointerUp(x, y, button, mods);
    } else {
        (void)x; (void)y; (void)mods;
        if (button == 0) ed.onMouseUp();
    }
}

template <typename E>
void dispatchMotion(E& ed, float x, float y, uint32_t mods) {
    if constexpr (HasPointerMove<E>::value) ed.onPointerMove(x, y, mods);
    else ed.onMouseMove(x, y, (mods & kModShift) != 0);
}

template <typename E>
void dispatchScroll(E& ed, float x, float y, float dx, float dy, uint32_t mods) {
    if constexpr (HasWheel<E>::value) ed.onWheel(x, y, dx, dy, mods);
    else { (void)dx; (void)mods; ed.onScroll(x, y, dy); }
}

// Returns true when the editor took the raw key, so the EditorKey mapping
// below is skipped. An editor that handles only a few extra keys returns
// false for the rest and keeps the shared Tab/arrow behaviour for free.
template <typename E>
bool dispatchKeyCode(E& ed, uint32_t key, uint32_t mods) {
    if constexpr (HasKeyCode<E>::value) return ed.onKeyCode(key, mods);
    else { (void)ed; (void)key; (void)mods; return false; }
}

template <typename E>
void dispatchKeyRelease(E& ed, uint32_t key, uint32_t mods) {
    if constexpr (HasKeyRelease<E>::value) ed.onKeyRelease(key, mods);
    else { (void)ed; (void)key; (void)mods; }
}

template <typename E>
void dispatchText(E& ed, uint32_t codepoint) {
    if constexpr (HasText<E>::value) ed.onText(codepoint);
    else { (void)ed; (void)codepoint; }
}

template <typename E>
void dispatchEnter(E& ed) {
    if constexpr (HasPointerEnter<E>::value) ed.onPointerEnter();
    else (void)ed;
}

// The basic rule -- a pointer that leaves mid-drag ends the drag, so the
// host's automation gesture cannot be left open -- stays for editors that do
// not say otherwise. One that declares onPointerLeave takes on that duty.
template <typename E>
void dispatchLeave(E& ed) {
    if constexpr (HasPointerLeave<E>::value) ed.onPointerLeave();
    else if (ed.dragging()) ed.onMouseUp();
}

template <typename E>
bool wantedCursor(const E& ed, Cursor& out) {
    if constexpr (HasCursor<E>::value) { out = ed.cursor(); return true; }
    else { (void)ed; (void)out; return false; }
}
}  // namespace

// Matches PuglEventFunc exactly, so no function-pointer cast is needed.
static PuglStatus eventFunc(PuglView* view, const PuglEvent* event) {
    if (auto* self = (Window*)puglGetHandle(view)) self->handleEvent(event);
    return PUGL_SUCCESS;
}

Window::~Window() { destroy(); }

// ---------------------------------------------------------------------------

bool Window::create() {
    if (view_) return true;

    world_ = puglNewWorld(standalone_ ? PUGL_PROGRAM : PUGL_MODULE, 0);
    if (!world_) return false;
    puglSetWorldString(world_, PUGL_CLASS_NAME, "ProbeSynth");

    view_ = puglNewView(world_);
    if (!view_) {
        puglFreeWorld(world_);
        world_ = nullptr;
        return false;
    }

    puglSetHandle(view_, this);
    puglSetEventFunc(view_, &eventFunc);

#if PROBESYNTH_METAL
    // Metal manages its own drawable, depth and stencil, so none of the pixel
    // format hints below apply -- nanovg_mtl sizes the stencil from the render
    // target itself.
    puglSetBackend(view_, puglMetalBackend());
#else
    puglSetBackend(view_, puglGlBackend());

    // GL2 compatibility: NanoVG's GL2 backend is what NanoVGBackend.c builds,
    // and it needs the fixed-function-era context to be available.
    puglSetViewHint(view_, PUGL_CONTEXT_API, PUGL_OPENGL_API);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MAJOR, 2);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MINOR, 0);
    puglSetViewHint(view_, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_COMPATIBILITY_PROFILE);
    // NVG_STENCIL_STROKES needs a stencil buffer; without this the strokes on
    // overlapping paths double-blend at the joins.
    puglSetViewHint(view_, PUGL_STENCIL_BITS, 8);
    puglSetViewHint(view_, PUGL_DEPTH_BITS, 0);
    puglSetViewHint(view_, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
    puglSetViewHint(view_, PUGL_SWAP_INTERVAL, 1);
#endif
    puglSetViewHint(view_, PUGL_RESIZABLE, PUGL_TRUE);
    puglSetViewHint(view_, PUGL_IGNORE_KEY_REPEAT, PUGL_TRUE);

    puglSetSizeHint(view_, PUGL_DEFAULT_SIZE, (PuglSpan)width_, (PuglSpan)height_);
    puglSetSizeHint(view_, PUGL_CURRENT_SIZE, (PuglSpan)width_, (PuglSpan)height_);
    // The panel is a fixed composition; letting it stretch would just tear the
    // layout apart, so the aspect is pinned and only the scale varies.
    puglSetSizeHint(view_, PUGL_MIN_SIZE, (PuglSpan)(kDesignW * 0.6f), (PuglSpan)(kDesignH * 0.6f));
    puglSetSizeHint(view_, PUGL_MAX_SIZE, (PuglSpan)(kDesignW * 2.0f), (PuglSpan)(kDesignH * 2.0f));
    puglSetSizeHint(view_, PUGL_FIXED_ASPECT, (PuglSpan)kDesignW, (PuglSpan)kDesignH);
    return true;
}

void Window::destroy() {
    if (view_ && fallbackTimer_) {
        puglStopTimer(view_, kFallbackTimerId);
        fallbackTimer_ = false;
    }
    if (view_) {
        puglFreeView(view_);
        view_ = nullptr;
    }
    if (world_) {
        puglFreeWorld(world_);
        world_ = nullptr;
    }
    vg_ = nullptr;
    realized_ = false;
    haveDrawTime_ = false;
}

bool Window::embed(uintptr_t parent) {
    if (!view_ && !create()) return false;
    if (realized_) return true;

    if (parent) puglSetParent(view_, (PuglNativeView)parent);
    return puglRealize(view_) == PUGL_SUCCESS;
}

bool Window::show() {
    if (!view_) return false;
    return puglShow(view_, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS;
}

bool Window::hide() {
    if (!view_) return false;
    return puglHide(view_) == PUGL_SUCCESS;
}

bool Window::setSize(uint32_t w, uint32_t h) {
    width_ = std::max(1u, w);
    height_ = std::max(1u, h);
    if (!view_) return true;
    return puglSetSizeHint(view_, PUGL_CURRENT_SIZE, (PuglSpan)width_, (PuglSpan)height_) ==
           PUGL_SUCCESS;
}

// Deliberately ignored, on every platform. The panel is drawn in design units
// and scaled by the ratio of the actual view size to those units, so the scale
// the host suggests is already implied by the size it gives us -- applying it
// again would double-count. Kept as a no-op rather than dropped from the
// vtable so hosts that expect the call still get a success.
void Window::setScale(double) {}

// NSTimer on macOS and SetTimer on Windows, both driven by the run loop the
// host is already spinning, so this works even when the host tells us nothing.
// It is deliberately NOT a fallback on X11: pugl's X11 timers are delivered
// from inside puglUpdate, which is the very call that would be missing. A Linux
// host that refuses clap.timer-support would therefore get a static editor.
// That has not been observed, but it is an assumption rather than a guarantee,
// and it is the first thing to check if a Linux editor ever fails to update.
bool Window::startFallbackTimer(double seconds) {
    if (!view_ || fallbackTimer_) return false;
    if (puglStartTimer(view_, kFallbackTimerId, seconds) != PUGL_SUCCESS) return false;
    fallbackTimer_ = true;
    return true;
}

void Window::stopFallbackTimer() {
    if (!view_ || !fallbackTimer_) return;
    puglStopTimer(view_, kFallbackTimerId);
    fallbackTimer_ = false;
}

void Window::tick() {
    if (!world_) return;
    // Zero timeout: the host owns the loop, and blocking here would stall it.
    // The repaint request itself belongs in the PUGL_UPDATE handler rather
    // than here; see there for why the difference is a factor of two.
    puglUpdate(world_, 0.0);
}

// ---------------------------------------------------------------------------

void Window::handleEvent(const void* eventPtr) {
    const PuglEvent* e = (const PuglEvent*)eventPtr;

    // The GL context is active for REALIZE, UNREALIZE and EXPOSE, and only
    // those, so the NanoVG context has to be built and torn down there.
    switch (e->type) {
        case PUGL_REALIZE:
            if (!probesynthLoadGL()) break;
            // Under GL this is the NSOpenGLContext/GLXContext and is ignored;
            // under Metal it is the CAMetalLayer, which nvgCreateMTL requires.
            vg_ = probesynthNvgCreate(puglGetContext(view_));
            if (!vg_) break;
            // A context without fonts draws shapes and no text; refuse it.
            if (!editor_.attach(vg_)) {
                probesynthNvgDelete(vg_);
                vg_ = nullptr;
                break;
            }
            editor_.markDirty();
            realized_ = true;
            haveDrawTime_ = false;
            break;

        case PUGL_UNREALIZE:
            if (vg_) {
                probesynthNvgDelete(vg_);
                vg_ = nullptr;
            }
            // Font handles died with the context; the next attach() must
            // re-upload them or every text draw silently disappears.
            editor_.onContextLost();
            realized_ = false;
            haveDrawTime_ = false;
            break;

        case PUGL_UPDATE:
            // pugl dispatches this from inside puglUpdate, immediately before
            // the expose phase, precisely so an application can mark itself
            // dirty in time to be drawn in the same cycle. Asked for here, the
            // obscure is merged into this update and rendered now.
            //
            // Asked for from tick() instead -- outside any update -- pugl
            // takes its other branch and posts an X expose to be picked up
            // "next time", so the frame is drawn one whole tick later. Every
            // frame then costs two ticks and X11 renders at half the timer
            // rate: measured at 98 renders per 200 ticks before this moved.
            // Cocoa hides the mistake because setNeedsDisplay is coalesced
            // into the same run-loop turn, which is why only Linux looked slow.
            //
            // Repaint only when something on screen could have changed:
            // telemetry pending, a gesture in flight, or a parameter moved. An
            // idle editor -- bypassed plugin, transport stopped, nobody
            // touching it -- still costs nothing.
            if (realized_ && editor_.wantsRepaint()) puglObscureView(view_);
            break;

        case PUGL_CONFIGURE:
            width_ = e->configure.width;
            height_ = e->configure.height;
            editor_.markDirty();
            break;

        case PUGL_EXPOSE:
            draw();
            break;

        case PUGL_TIMER:
            // Delivered from the host's run loop rather than from inside
            // puglUpdate, so ask for a repaint instead of pumping -- pumping
            // here would re-enter the event loop.
            if (e->timer.id == kFallbackTimerId && realized_ && editor_.wantsRepaint())
                puglObscureView(view_);
            break;

        case PUGL_BUTTON_PRESS: {
            puglGrabFocus(view_);
            const float x = (float)e->button.x, y = (float)e->button.y;
            // A second press of any button within the window counts; the
            // dispatcher decides which buttons the editor gets to see.
            const bool dbl = (e->button.time - lastClickTime_) < kDoubleClickSeconds &&
                             std::fabs(x - lastClickX_) < kDoubleClickSlop &&
                             std::fabs(y - lastClickY_) < kDoubleClickSlop;
            lastClickTime_ = e->button.time;
            lastClickX_ = x;
            lastClickY_ = y;

            const float sx = x / (float)scaleX(), sy = y / (float)scaleY();
            dispatchPress(editor_, sx, sy, (int)e->button.button, modsOf(e->button.state), dbl);
            syncCursor();
            break;
        }

        case PUGL_BUTTON_RELEASE:
            dispatchRelease(editor_, (float)(e->button.x / scaleX()),
                            (float)(e->button.y / scaleY()), (int)e->button.button,
                            modsOf(e->button.state));
            syncCursor();
            break;

        case PUGL_MOTION:
            dispatchMotion(editor_, (float)(e->motion.x / scaleX()),
                           (float)(e->motion.y / scaleY()), modsOf(e->motion.state));
            syncCursor();
            break;

        case PUGL_SCROLL:
            dispatchScroll(editor_, (float)(e->scroll.x / scaleX()),
                           (float)(e->scroll.y / scaleY()), (float)e->scroll.dx,
                           (float)e->scroll.dy, modsOf(e->scroll.state));
            break;

        case PUGL_POINTER_IN:
            dispatchEnter(editor_);
            syncCursor();
            break;

        case PUGL_POINTER_OUT:
            // Losing the pointer mid-drag would otherwise leave the host's
            // automation gesture open forever; see dispatchLeave.
            dispatchLeave(editor_);
            break;

        case PUGL_KEY_PRESS: {
            const uint32_t mods = modsOf(e->key.state);
            if (dispatchKeyCode(editor_, e->key.key, mods)) break;
            EditorKey key = EditorKey::Tab;
            bool handled = true;
            switch (e->key.key) {
                case PUGL_KEY_TAB: key = EditorKey::Tab; break;
                case PUGL_KEY_LEFT: key = EditorKey::Left; break;
                case PUGL_KEY_RIGHT: key = EditorKey::Right; break;
                case PUGL_KEY_UP: key = EditorKey::Up; break;
                case PUGL_KEY_DOWN: key = EditorKey::Down; break;
                case PUGL_KEY_HOME: key = EditorKey::Home; break;
                case PUGL_KEY_END: key = EditorKey::End; break;
                case PUGL_KEY_ENTER: key = EditorKey::Enter; break;
                case PUGL_KEY_SPACE: key = EditorKey::Space; break;
                default: handled = false; break;
            }
            if (handled) editor_.onKey(key, (mods & kModShift) != 0);
            break;
        }

        case PUGL_KEY_RELEASE:
            dispatchKeyRelease(editor_, e->key.key, modsOf(e->key.state));
            break;

        case PUGL_TEXT:
            dispatchText(editor_, e->text.character);
            break;

        case PUGL_FOCUS_OUT:
            editor_.onFocusLost();
            break;

        default:
            break;
    }
}

// Asks an editor that has an opinion about the cursor what it wants now, and
// tells pugl only when that changed. Nothing happens for an editor without
// cursor(); the arrow it never asked to change stays.
void Window::syncCursor() {
    Cursor wanted = Cursor::Arrow;
    if (!view_ || !wantedCursor(editor_, wanted) || wanted == cursor_) return;
    cursor_ = wanted;
    puglSetCursor(view_, puglCursorOf(wanted));
}

void Window::draw() {
    if (!vg_) return;

    const auto now = std::chrono::steady_clock::now();
    const double elapsed =
        haveDrawTime_ ? std::chrono::duration<double>(now - lastDrawTime_).count() : 1.0 / 60.0;
    lastDrawTime_ = now;
    haveDrawTime_ = true;
    editor_.pump(elapsed);

    // macOS hands us a high-resolution GL surface but reports logical
    // coordinates, so the framebuffer is backingScaleFactor times bigger.
    // X11 and Win32 report physical pixels and need no correction.
#if defined(__APPLE__)
    const float pixelRatio = (float)puglGetScaleFactor(view_);
#else
    const float pixelRatio = 1.0f;
#endif

    probesynthViewport((int)((float)width_ * pixelRatio), (int)((float)height_ * pixelRatio));
    const NVGcolor bg = rgb(col::ink);
    probesynthClear(vg_, bg.r, bg.g, bg.b);

    nvgBeginFrame(vg_, (float)width_, (float)height_, pixelRatio);
    // One transform takes the whole panel from design units to the view. Every
    // coordinate in Editor.cpp is therefore a design-space constant, and
    // nothing below has to think about DPI or host scaling.
    nvgScale(vg_, (float)scaleX(), (float)scaleY());
    editor_.render(vg_);
    nvgEndFrame(vg_);

    if (onFrameDrawn) onFrameDrawn(frameUser);
}

}  // namespace probesynth::gui
