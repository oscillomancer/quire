// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Window.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <pugl/pugl.h>

#if SUPPLE_METAL
#include "gui/pugl_metal.h"
#else
#include <pugl/gl.h>
#endif

#include "gui/Canvas.h"
#include "gui/EditorBase.h"
#include "gui/Theme.h"

namespace supple::gui {
namespace {

constexpr uintptr_t kFallbackTimerId = 1;
constexpr double kDoubleClickSeconds = 0.4;
constexpr float kDoubleClickSlop = 4.0f;

uint32_t modsOf(PuglMods state) {
    uint32_t m = 0;
    if (state & PUGL_MOD_SHIFT) m |= mod::kShift;
    if (state & PUGL_MOD_CTRL) m |= mod::kCtrl;
    if (state & PUGL_MOD_ALT) m |= mod::kAlt;
    if (state & PUGL_MOD_SUPER) m |= mod::kSuper;
    return m;
}

PuglCursor puglCursorOf(Cursor c) {
    switch (c) {
        case Cursor::Hand: return PUGL_CURSOR_HAND;
        case Cursor::Caret: return PUGL_CURSOR_CARET;
        case Cursor::LeftRight: return PUGL_CURSOR_LEFT_RIGHT;
        case Cursor::UpDown: return PUGL_CURSOR_UP_DOWN;
        case Cursor::Move: return PUGL_CURSOR_ALL_SCROLL;
        case Cursor::Arrow: break;
    }
    return PUGL_CURSOR_ARROW;
}

PuglStatus eventThunk(PuglView* view, const PuglEvent* event) {
    if (auto* window = static_cast<Window*>(puglGetHandle(view))) window->handleEvent(event);
    return PUGL_SUCCESS;
}

}  // namespace

Window::Window(EditorBase& editor, const char* className, bool standalone)
    : editor_(editor),
      className_(className),
      standalone_(standalone),
      hostW_(static_cast<uint32_t>(std::lround(editor.width()))),
      hostH_(static_cast<uint32_t>(std::lround(editor.height()))),
      width_(hostW_),
      height_(hostH_) {}

Window::~Window() { destroy(); }

bool Window::create() {
    if (view_) return true;
    world_ = puglNewWorld(standalone_ ? PUGL_PROGRAM : PUGL_MODULE, 0);
    if (!world_) return false;
    puglSetWorldString(world_, PUGL_CLASS_NAME, className_);
    view_ = puglNewView(world_);
    if (!view_) {
        puglFreeWorld(world_);
        world_ = nullptr;
        return false;
    }
    puglSetHandle(view_, this);
    puglSetEventFunc(view_, eventThunk);
    width_ = toPixels(hostW_);
    height_ = toPixels(hostH_);

#if SUPPLE_METAL
    // Metal manages its own drawable, depth and stencil; nanovg_mtl sizes
    // the stencil from the render target.
    puglSetBackend(view_, puglMetalBackend());
#else
    puglSetBackend(view_, puglGlBackend());
    // NanoVG's GL2 backend wants a compatibility context, and its stencil
    // strokes want a stencil buffer: without one overlapping strokes
    // double-blend at the joins.
    puglSetViewHint(view_, PUGL_CONTEXT_API, PUGL_OPENGL_API);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MAJOR, 2);
    puglSetViewHint(view_, PUGL_CONTEXT_VERSION_MINOR, 0);
    puglSetViewHint(view_, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_COMPATIBILITY_PROFILE);
    puglSetViewHint(view_, PUGL_STENCIL_BITS, 8);
    puglSetViewHint(view_, PUGL_DEPTH_BITS, 0);
    puglSetViewHint(view_, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
    puglSetViewHint(view_, PUGL_SWAP_INTERVAL, 1);
#endif
    // Embedded, the host sizes us. A standalone window (the preview tool) is
    // fixed, so a tiling window manager cannot reshape it.
    puglSetViewHint(view_, PUGL_RESIZABLE, standalone_ ? PUGL_FALSE : PUGL_TRUE);
    // Repeat is wanted: holding an arrow key is one gesture, which Ui closes
    // a quarter second after the last repeat.
    puglSetViewHint(view_, PUGL_IGNORE_KEY_REPEAT, PUGL_FALSE);
    // Files dropped on the editor arrive as a URI list on every platform.
    puglSetViewHint(view_, PUGL_ACCEPT_DROP, PUGL_TRUE);
    puglRegisterDropType(view_, "text/uri-list");
    puglSetSizeHint(view_, PUGL_DEFAULT_SIZE, static_cast<PuglSpan>(width_), static_cast<PuglSpan>(height_));
    puglSetSizeHint(view_, PUGL_CURRENT_SIZE, static_cast<PuglSpan>(width_), static_cast<PuglSpan>(height_));
    if (minW_ && maxW_) setSizeLimits(minW_, minH_, maxW_, maxH_);
    // The panel is one composition: only its scale varies, never its shape.
    puglSetSizeHint(view_, PUGL_FIXED_ASPECT, static_cast<PuglSpan>(std::lround(editor_.width())),
                    static_cast<PuglSpan>(std::lround(editor_.height())));
    return true;
}

void Window::destroy() {
    if (view_ && fallbackTimer_) puglStopTimer(view_, kFallbackTimerId);
    fallbackTimer_ = false;
    if (view_) {
        puglFreeView(view_);  // dispatches UNREALIZE while the editor still exists
        view_ = nullptr;
    }
    if (world_) {
        puglFreeWorld(world_);
        world_ = nullptr;
    }
    vg_ = nullptr;
    realized_ = false;
    haveIdle_ = false;
}

bool Window::embed(uintptr_t parent) {
    if (!view_ && !create()) return false;
    if (realized_) return true;
    if (parent) puglSetParent(view_, static_cast<PuglNativeView>(parent));
    if (puglRealize(view_) == PUGL_SUCCESS && realized_) return true;
    // No view, or one without a renderer or fonts: a blank editor is not a
    // success. Tear it down so a later attempt starts clean.
    destroy();
    return false;
}

bool Window::show() { return view_ && puglShow(view_, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS; }

bool Window::hide() { return view_ && puglHide(view_) == PUGL_SUCCESS; }

double Window::pixelsPerUnit() const {
#if defined(__APPLE__)
    // Before realizing, pugl answers for the main screen, and converts with
    // that same factor, so the size it creates is still the one asked for.
    return view_ ? std::max(0.5, puglGetScaleFactor(view_)) : 1.0;
#else
    return 1.0;
#endif
}

uint32_t Window::pixels(uint32_t hostUnits, double scale) {
    // In double throughout: a host's size times a 2x scale can pass 32 bits,
    // and lround's long is 32 bits on Windows.
    const double p = std::round(static_cast<double>(hostUnits) * scale);
    return p <= kMaxSpan ? static_cast<uint32_t>(p) : kMaxSpan + 1;
}

uint32_t Window::toPixels(uint32_t hostUnits) const { return pixels(hostUnits, pixelsPerUnit()); }

bool Window::setSize(uint32_t w, uint32_t h) {
    // A larger size would wrap PuglSpan, not clip.
    if (w == 0 || h == 0 || toPixels(w) > kMaxSpan || toPixels(h) > kMaxSpan) return false;
    // Pugl applies CURRENT_SIZE only to a view that already has a native
    // window, and fails it otherwise; a view is realized at DEFAULT_SIZE.
    // Between create() and embed() -- where a host may well call set_size,
    // and where a saved editor scale is applied -- the size therefore has to
    // go to DEFAULT_SIZE, or it is refused and the editor opens at 1x.
    const PuglSizeHint hint = view_ && puglGetNativeView(view_) ? PUGL_CURRENT_SIZE : PUGL_DEFAULT_SIZE;
    if (view_ && puglSetSizeHint(view_, hint, static_cast<PuglSpan>(toPixels(w)),
                                 static_cast<PuglSpan>(toPixels(h))) != PUGL_SUCCESS)
        return false;
    hostW_ = w;
    hostH_ = h;
    width_ = toPixels(w);
    height_ = toPixels(h);
    editor_.markDirty();
    return true;
}

void Window::setSizeLimits(uint32_t minW, uint32_t minH, uint32_t maxW, uint32_t maxH) {
    minW_ = minW;
    minH_ = minH;
    maxW_ = maxW;
    maxH_ = maxH;
    if (!view_) return;
#if defined(__APPLE__)
    // This pugl turns the limits into layout constraints without converting
    // pixels to points, which on a 2x screen would double them. Embedded,
    // the host enforces limits through adjust_size; the preview is fixed.
#else
    const auto span = [](uint32_t v) { return static_cast<PuglSpan>(std::min<uint32_t>(v, 0xFFFF)); };
    puglSetSizeHint(view_, PUGL_MIN_SIZE, span(minW), span(minH));
    puglSetSizeHint(view_, PUGL_MAX_SIZE, span(maxW), span(maxH));
#endif
}

// NSTimer on macOS and SetTimer on Windows, both driven by the run loop the
// host already spins, so this works even when the host offers no timer.
// Deliberately not a fallback on X11: pugl's X11 timers are delivered from
// inside puglUpdate, which is exactly the call that would be missing. A Linux
// host that refuses clap.timer-support gets a static editor; none is known to.
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

// Zero timeout: the host owns the loop, and blocking would stall it. The
// repaint is requested from PUGL_UPDATE, inside this call, rather than here:
// asked for outside an update, X11 pugl posts an expose for "next time" and
// every frame lands one tick late, halving the frame rate.
void Window::tick() {
    if (world_) puglUpdate(world_, 0.0);
}

void Window::frame() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = haveIdle_ ? std::chrono::duration<double>(now - lastIdle_).count() : 1.0 / 60.0;
    lastIdle_ = now;
    haveIdle_ = true;
    if (files_) files_->poll();  // a dialog that finished hands over its paths
    editor_.idle(std::min(elapsed, 0.25));
    if (realized_ && editor_.needsRepaint()) puglObscureView(view_);
}

Window::Fit Window::fit() const {
    // Uniform scale, centred: a host that ignores the aspect hint gets a
    // letterboxed panel rather than a stretched one.
    const float w = static_cast<float>(std::max(1u, width_)), h = static_cast<float>(std::max(1u, height_));
    const float s = std::min(w / editor_.width(), h / editor_.height());
    return {s, (w - editor_.width() * s) * 0.5f, (h - editor_.height() * s) * 0.5f};
}

float Window::toDesignX(double x) const {
    const Fit f = fit();
    return (static_cast<float>(x) - f.offsetX) / f.scale;
}
float Window::toDesignY(double y) const {
    const Fit f = fit();
    return (static_cast<float>(y) - f.offsetY) / f.scale;
}

void Window::designToHost(float x, float y, int32_t& hostX, int32_t& hostY) const {
    const Fit f = fit();
    const double perUnit = pixelsPerUnit();
    hostX = static_cast<int32_t>(std::lround((x * f.scale + f.offsetX) / perUnit));
    hostY = static_cast<int32_t>(std::lround((y * f.scale + f.offsetY) / perUnit));
}

bool Window::copy(std::string_view text) {
    return view_ && realized_ &&
           puglSetClipboard(view_, PUGL_CLIPBOARD_GENERAL, "text/plain", text.data(), text.size()) == PUGL_SUCCESS;
}

bool Window::paste(std::function<void(std::string_view)> done) {
    if (!view_ || !realized_) return false;
    paste_ = std::move(done);
    if (puglPaste(view_) == PUGL_SUCCESS) return true;
    paste_ = nullptr;
    return false;
}

void Window::handleEvent(const void* eventPointer) {
    const PuglEvent* e = static_cast<const PuglEvent*>(eventPointer);
    switch (e->type) {
        // The graphics context is current for REALIZE, UNREALIZE and EXPOSE
        // only, so the NanoVG context is built and torn down here.
        case PUGL_REALIZE:
            editor_.setClipboard(this);
            if (!canvasLoad()) break;
            vg_ = canvasCreate(puglGetContext(view_));
            if (!vg_) break;
            if (!editor_.attach(vg_)) {  // a context without fonts draws no text: refuse it
                canvasDestroy(vg_);
                vg_ = nullptr;
                break;
            }
            realized_ = true;
            haveIdle_ = false;
            files_ = std::make_unique<NativeFiles>(puglGetNativeView(view_));
            editor_.setFileDialogs(files_.get());
            break;

        case PUGL_UNREALIZE:
            editor_.setClipboard(nullptr);
            editor_.setFileDialogs(nullptr);
            files_.reset();  // a dialog still open closes with the editor
            paste_ = nullptr;
            editor_.detach();
            if (vg_) canvasDestroy(vg_);
            vg_ = nullptr;
            realized_ = false;
            break;

        case PUGL_UPDATE: frame(); break;

        case PUGL_CONFIGURE: {
            width_ = std::max<uint32_t>(1, e->configure.width);
            height_ = std::max<uint32_t>(1, e->configure.height);
            const double ppu = pixelsPerUnit();
            hostW_ = static_cast<uint32_t>(std::lround(width_ / ppu));
            hostH_ = static_cast<uint32_t>(std::lround(height_ / ppu));
            editor_.markDirty();
            break;
        }

        case PUGL_EXPOSE: draw(); break;

        case PUGL_TIMER:
            // Delivered from the host's run loop, not from inside puglUpdate,
            // so idle and ask for a repaint; never pump from here.
            if (e->timer.id == kFallbackTimerId) frame();
            break;

        case PUGL_BUTTON_PRESS: {
            const float x = toDesignX(e->button.x), y = toDesignY(e->button.y);
            const int button = static_cast<int>(e->button.button);
            const bool doubleClick = button == lastPressButton_ && e->button.time - lastPressTime_ < kDoubleClickSeconds &&
                                     std::fabs(x - lastPressX_) < kDoubleClickSlop &&
                                     std::fabs(y - lastPressY_) < kDoubleClickSlop;
            // A third press is a new first press, not a second double-click.
            lastPressTime_ = doubleClick ? -1.0 : e->button.time;
            lastPressX_ = x;
            lastPressY_ = y;
            lastPressButton_ = button;
            // Keyboard focus only when the press landed on something that can
            // use it, so a stray click does not take the host's shortcuts.
            if (editor_.pointerDown(x, y, button, modsOf(e->button.state), doubleClick)) puglGrabFocus(view_);
            syncCursor();
            break;
        }

        case PUGL_BUTTON_RELEASE:
            editor_.pointerUp(toDesignX(e->button.x), toDesignY(e->button.y), static_cast<int>(e->button.button),
                              modsOf(e->button.state));
            syncCursor();
            break;

        case PUGL_MOTION:
            editor_.pointerMove(toDesignX(e->motion.x), toDesignY(e->motion.y), modsOf(e->motion.state));
            syncCursor();
            break;

        case PUGL_SCROLL:
            editor_.wheel(toDesignX(e->scroll.x), toDesignY(e->scroll.y), static_cast<float>(e->scroll.dx),
                          static_cast<float>(e->scroll.dy), modsOf(e->scroll.state));
            break;

        case PUGL_POINTER_IN: syncCursor(); break;
        case PUGL_POINTER_OUT: editor_.pointerLeave(); break;

        case PUGL_KEY_PRESS: editor_.key(e->key.key, modsOf(e->key.state)); break;
        case PUGL_TEXT: editor_.text(e->text.character); break;
        case PUGL_FOCUS_OUT: editor_.focusLost(); break;

        case PUGL_DATA_OFFER: {
            const PuglDataOfferEvent& offer = e->offer;
            if (offer.clipboard == PUGL_CLIPBOARD_DRAG) {
                // Files, where the editor takes them.
                const uint32_t types = puglGetNumClipboardTypes(view_, offer.clipboard);
                bool accepted = false;
                for (uint32_t i = 0; i < types && !accepted; ++i) {
                    const char* type = puglGetClipboardType(view_, offer.clipboard, i);
                    if (type && !std::strcmp(type, "text/uri-list") &&
                        editor_.dropOver(toDesignX(offer.x), toDesignY(offer.y)))
                        accepted = puglAcceptOffer(view_, &offer, i, PUGL_DATA_ACTION_COPY, 0, 0, width_, height_) ==
                                   PUGL_SUCCESS;
                }
                if (!accepted) puglRejectOffer(view_, &offer, 0, 0, width_, height_);
                break;
            }
            // Only text, and only when the editor asked for it: a stray
            // offer is refused.
            const uint32_t types = puglGetNumClipboardTypes(view_, offer.clipboard);
            bool accepted = false;
            for (uint32_t i = 0; paste_ && offer.clipboard == PUGL_CLIPBOARD_GENERAL && i < types && !accepted; ++i) {
                const char* type = puglGetClipboardType(view_, offer.clipboard, i);
                if (type && !std::strncmp(type, "text/plain", 10))
                    accepted = puglAcceptOffer(view_, &offer, i, PUGL_DATA_ACTION_COPY, 0, 0, width_, height_) ==
                               PUGL_SUCCESS;
            }
            if (!accepted) {
                puglRejectOffer(view_, &offer, 0, 0, width_, height_);
                paste_ = nullptr;
            }
            break;
        }

        case PUGL_DATA: {
            size_t length = 0;
            const void* data = puglGetClipboard(view_, e->data.clipboard, e->data.typeIndex, &length);
            if (e->data.clipboard == PUGL_CLIPBOARD_DRAG) {
                if (data)
                    editor_.drop(toDesignX(e->data.x), toDesignY(e->data.y),
                                 pathsFromUriList(std::string_view(static_cast<const char*>(data), length)));
                break;
            }
            auto done = std::move(paste_);
            paste_ = nullptr;
            if (done && data) {
                std::string_view text(static_cast<const char*>(data), length);
                while (!text.empty() && text.back() == '\0') text.remove_suffix(1);
                done(text);
                editor_.markDirty();
            }
            break;
        }

        default: break;
    }
}

void Window::syncCursor() {
    const int wanted = static_cast<int>(editor_.cursor());
    if (!view_ || wanted == cursor_) return;
    cursor_ = wanted;
    puglSetCursor(view_, puglCursorOf(static_cast<Cursor>(wanted)));
}

void Window::draw() {
    if (!vg_) return;
    // Pugl's sizes are backing pixels on every platform, the drawable's
    // too, so the frame is laid out in pixels with a ratio of 1; the backing
    // scale is already in width_ and height_, and in fit().
    canvasViewport(static_cast<int>(width_), static_cast<int>(height_));
    const NVGcolor bg = rgb(editor_.background());
    canvasClear(vg_, bg.r, bg.g, bg.b);
    nvgBeginFrame(vg_, static_cast<float>(width_), static_cast<float>(height_), 1.0f);
    // One transform from design units to the view; nothing in the editor
    // ever thinks about pixels, DPI or host scaling.
    const Fit f = fit();
    nvgTranslate(vg_, f.offsetX, f.offsetY);
    nvgScale(vg_, f.scale, f.scale);
    editor_.render(vg_);
    nvgEndFrame(vg_);
    if (onFrameDrawn) onFrameDrawn(frameUser);
}

}  // namespace supple::gui
