// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Offscreen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gui/EditorBase.h"

#include "gui/Canvas.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#if !SUPPLE_METAL
#include <OpenGL/OpenGL.h>
#endif
#else
#include <dlfcn.h>
#endif

namespace supple::gui {

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)

// EGL as far as this file uses it, declared here rather than taken from
// <EGL/egl.h>, which a build machine need not have: these types and numbers
// are fixed by the Khronos registry.
using EGLBoolean = unsigned int;
using EGLint = int32_t;
using EGLenum = unsigned int;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
constexpr EGLenum kPlatformSurfacelessMesa = 0x31DD;
constexpr EGLenum kOpenGlApi = 0x30A2;
constexpr EGLint kNone = 0x3038, kSurfaceType = 0x3033, kPbufferBit = 0x0001, kRenderableType = 0x3040,
                 kOpenGlBit = 0x0008, kWidth = 0x3057, kHeight = 0x3056, kExtensions = 0x3055;

struct Egl {
    void* (*getProcAddress)(const char*) = nullptr;
    EGLDisplay (*getDisplay)(void*) = nullptr;
    EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*terminate)(EGLDisplay) = nullptr;
    EGLBoolean (*bindApi)(EGLenum) = nullptr;
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLSurface (*createPbuffer)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
    EGLBoolean (*destroySurface)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    const char* (*queryString)(EGLDisplay, EGLint) = nullptr;
    EGLBoolean (*releaseThread)() = nullptr;
    std::string problem;  // why it would not load, if it would not
};

template <class F>
bool find(void* lib, F& f, const char* name) {
    f = reinterpret_cast<F>(dlsym(lib, name));
    return f != nullptr;
}

bool hasExtension(const char* list, const char* name) {
    const size_t n = std::strlen(name);
    for (const char* p = list ? std::strstr(list, name) : nullptr; p; p = std::strstr(p + 1, name))
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

// Loaded once, for the life of the process; never unloaded (EGL keeps
// threads and state of its own).
const Egl& egl() {
    static Egl e;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            e.problem = "libEGL.so.1 is not installed";
            return;
        }
        if (!find(lib, e.getProcAddress, "eglGetProcAddress") || !find(lib, e.getDisplay, "eglGetDisplay") ||
            !find(lib, e.initialize, "eglInitialize") || !find(lib, e.terminate, "eglTerminate") ||
            !find(lib, e.bindApi, "eglBindAPI") ||
            !find(lib, e.chooseConfig, "eglChooseConfig") || !find(lib, e.createContext, "eglCreateContext") ||
            !find(lib, e.destroyContext, "eglDestroyContext") ||
            !find(lib, e.createPbuffer, "eglCreatePbufferSurface") ||
            !find(lib, e.destroySurface, "eglDestroySurface") || !find(lib, e.makeCurrent, "eglMakeCurrent") ||
            !find(lib, e.queryString, "eglQueryString") || !find(lib, e.releaseThread, "eglReleaseThread")) {
            e.problem = "libEGL.so.1 lacks EGL 1.4";
            e.getProcAddress = nullptr;
        }
    });
    return e;
}

// A display for one picture, initialised and terminated with it, so that
// nothing EGL made outlives the picture: no display server needed (Mesa's
// surfaceless platform), else whatever the default display is.
struct Display {
    const Egl& e;
    EGLDisplay display = nullptr;
    bool surfaceless = false;  // EGL_KHR_surfaceless_context
    bool noConfig = false;     // EGL_KHR_no_config_context

    explicit Display(const Egl& egl) : e(egl) {
        using PlatformDisplay = EGLDisplay (*)(EGLenum, void*, const EGLint*);
        EGLint major = 0, minor = 0;
        if (auto platform = reinterpret_cast<PlatformDisplay>(e.getProcAddress("eglGetPlatformDisplayEXT"))) {
            EGLDisplay d = platform(kPlatformSurfacelessMesa, nullptr, nullptr);
            if (d && e.initialize(d, &major, &minor)) display = d;
        }
        if (!display) {
            EGLDisplay d = e.getDisplay(nullptr);
            if (d && e.initialize(d, &major, &minor)) display = d;
        }
        if (!display) return;
        const char* extensions = e.queryString(display, kExtensions);
        surfaceless = hasExtension(extensions, "EGL_KHR_surfaceless_context");
        noConfig = hasExtension(extensions, "EGL_KHR_no_config_context");
    }
    ~Display() {
        if (display) e.terminate(display);
        e.releaseThread();
    }
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
};

#endif

// The platform state lives entirely on the worker thread, including every
// failure path. No context or window is left current on the caller's thread.
struct Context {
#if defined(_WIN32)
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC gl = nullptr;
    bool open(std::string& why) {
        // The system's own class avoids DLL-owned class registration and its
        // teardown hazards. WS_POPUP without WS_VISIBLE is never shown.
        window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window || !(dc = GetDC(window))) {
            why = "no offscreen drawing: WGL could not create a hidden window";
            return false;
        }
        PIXELFORMATDESCRIPTOR format{};
        format.nSize = sizeof(format);
        format.nVersion = 1;
        format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
        format.iPixelType = PFD_TYPE_RGBA;
        format.cColorBits = 32;
        format.cDepthBits = 24;
        format.cStencilBits = 8;
        const int pixelFormat = ChoosePixelFormat(dc, &format);
        if (!pixelFormat || !SetPixelFormat(dc, pixelFormat, &format)) {
            why = "no offscreen drawing: WGL has no OpenGL pixel format";
            return false;
        }
        gl = wglCreateContext(dc);
        if (!gl || !wglMakeCurrent(dc, gl)) {
            why = "no offscreen drawing: WGL could not make an OpenGL context current";
            return false;
        }
        return true;
    }
    ~Context() {
        if (gl) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(gl);
        }
        if (dc) ReleaseDC(window, dc);
        if (window) DestroyWindow(window);
    }
#elif defined(__APPLE__)
#if SUPPLE_METAL
    // CanvasMetal owns the device, queue and unattached layer, with the target.
    bool open(std::string&) { return true; }
#else
    // The optional GL preview/test backend still builds on macOS. The plugin
    // uses Metal by default; this does not add GL to the Metal plugin.
    CGLContextObj gl = nullptr;
    bool open(std::string& why) {
        const CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, kCGLPFAColorSize,
            static_cast<CGLPixelFormatAttribute>(32), kCGLPFADepthSize,
            static_cast<CGLPixelFormatAttribute>(24), kCGLPFAStencilSize,
            static_cast<CGLPixelFormatAttribute>(8), static_cast<CGLPixelFormatAttribute>(0)};
        CGLPixelFormatObj format = nullptr;
        GLint count = 0;
        const CGLError chosen = CGLChoosePixelFormat(attributes, &format, &count);
        const CGLError made = format ? CGLCreateContext(format, nullptr, &gl) : chosen;
        if (format) CGLDestroyPixelFormat(format);
        if (made != kCGLNoError || !gl || CGLSetCurrentContext(gl) != kCGLNoError) {
            why = "no offscreen drawing: CGL could not make an OpenGL context current";
            return false;
        }
        return true;
    }
    ~Context() {
        if (gl) {
            CGLSetCurrentContext(nullptr);
            CGLDestroyContext(gl);
        }
    }
#endif
#else
    const Egl& e = egl();
    // Construct Display only after the loader has succeeded.
    std::unique_ptr<Display> d;
    EGLContext gl = nullptr;
    EGLSurface surface = nullptr;
    bool open(std::string& why) {
        if (!e.getProcAddress) {
            why = "no offscreen drawing: " + e.problem;
            return false;
        }
        d = std::make_unique<Display>(e);
        if (!d->display) {
            why = "no offscreen drawing: EGL found no display, not even a surfaceless one";
            return false;
        }
        if (!e.bindApi(kOpenGlApi)) {
            why = "no offscreen drawing: EGL offers no desktop OpenGL";
            return false;
        }
        const EGLint wantPbuffer[] = {kSurfaceType, kPbufferBit, kRenderableType, kOpenGlBit, kNone};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        if (!e.chooseConfig(d->display, wantPbuffer, &config, 1, &configs) || configs < 1) config = nullptr;
        if (!config && !(d->surfaceless && d->noConfig)) {
            why = "no offscreen drawing: EGL has no configuration for OpenGL";
            return false;
        }
        gl = e.createContext(d->display, config, nullptr, nullptr);
        if (!gl) {
            why = "no offscreen drawing: EGL made no OpenGL context";
            return false;
        }
        if (!d->surfaceless && config) {
            const EGLint size[] = {kWidth, 1, kHeight, 1, kNone};
            surface = e.createPbuffer(d->display, config, size);
        }
        if (!e.makeCurrent(d->display, surface, surface, gl)) {
            why = "no offscreen drawing: the OpenGL context would not become current";
            return false;
        }
        return true;
    }
    ~Context() {
        if (!d || !gl) return;
        e.makeCurrent(d->display, nullptr, nullptr, nullptr);
        if (surface) e.destroySurface(d->display, surface);
        e.destroyContext(d->display, gl);
    }
#endif
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};

// The pointer input, as Window would pass it on, with the main button.
void apply(EditorBase& editor, std::span<const PointerInput> input) {
    for (const PointerInput& in : input) {
        const uint32_t mods =
            (in.shift ? mod::kShift : 0u) | (in.ctrl ? mod::kCtrl : 0u) | (in.alt ? mod::kAlt : 0u);
        switch (in.kind) {
            case PointerInput::Kind::Move: editor.pointerMove(in.x, in.y, mods); break;
            case PointerInput::Kind::Press: editor.pointerDown(in.x, in.y, 0, mods, in.doubleClick); break;
            case PointerInput::Kind::Release: editor.pointerUp(in.x, in.y, 0, mods); break;
            case PointerInput::Kind::Wheel: editor.wheel(in.x, in.y, 0.0f, in.wheel, mods); break;
            case PointerInput::Kind::Leave: editor.pointerLeave(); break;
        }
    }
}

// One picture, on the calling thread: a context, a framebuffer, the editor
// drawn into it, read back.
bool draw(EditorBase& editor, double scale, std::span<const PointerInput> input, Picture& out, std::string& why) {
    Context context;
    if (!context.open(why)) return false;

    const double s = std::clamp(std::isfinite(scale) ? scale : 1.0, 0.25, 4.0);
    const int w = std::max(1, static_cast<int>(std::lround(editor.width() * s)));
    const int h = std::max(1, static_cast<int>(std::lround(editor.height() * s)));
    const char* problem = nullptr;
    std::unique_ptr<CanvasTarget, decltype(&canvasTargetDestroy)> target(
        canvasTargetCreate(w, h, &problem), canvasTargetDestroy);
    if (!target) {
        why = problem;
        return false;
    }
    NVGcontext* vg = canvasTargetContext(target.get());
    if (!editor.attach(vg)) {
        why = "the editor's fonts would not load";
        return false;
    }
    editor.idle(0.0);  // telemetry drained: the meters show what played
    if (!input.empty()) {
        apply(editor, input);
        editor.idle(0.0);
    }
    canvasViewport(w, h);
    const NVGcolor bg = rgb(editor.background());
    canvasClear(vg, bg.r, bg.g, bg.b);
    nvgBeginFrame(vg, static_cast<float>(w), static_cast<float>(h), 1.0f);
    nvgScale(vg, static_cast<float>(s), static_cast<float>(s));
    editor.render(vg);
    nvgEndFrame(vg);
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    const bool ok = canvasReadPixelsRGBA(target.get(), w, h, out.rgba.data()) != 0;
    editor.focusLost();  // every gesture the input opened, ended
    editor.detach();
    if (!ok) {
        why = "no offscreen drawing: render target readback failed";
        out = Picture{};
    }
    return ok;
}

}  // namespace

bool drawOffscreen(EditorBase& editor, double scale, std::span<const PointerInput> input, Picture& out,
                   std::string* why) {
    out = Picture{};
    std::string problem;
    bool ok = false;
    // A thread of its own: whatever context the caller has stays current.
    std::thread worker([&] { ok = draw(editor, scale, input, out, problem); });
    worker.join();
    if (!ok && why) *why = problem;
    return ok;
}

}  // namespace supple::gui
