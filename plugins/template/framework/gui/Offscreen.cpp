// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Offscreen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gui/EditorBase.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <dlfcn.h>

#include "gui/Canvas.h"
#define SUPPLE_OFFSCREEN_EGL 1
#else
#define SUPPLE_OFFSCREEN_EGL 0
#endif

namespace supple::gui {

#if SUPPLE_OFFSCREEN_EGL
namespace {

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

CanvasProc resolveGl(const char* name) { return reinterpret_cast<CanvasProc>(egl().getProcAddress(name)); }

// One picture, on the calling thread: a context, a framebuffer, the editor
// drawn into it, read back.
bool draw(EditorBase& editor, double scale, Picture& out, std::string& why) {
    const Egl& e = egl();
    if (!e.getProcAddress) {
        why = "no offscreen drawing: " + e.problem;
        return false;
    }
    Display d(e);
    if (!d.display) {
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
    if (!e.chooseConfig(d.display, wantPbuffer, &config, 1, &configs) || configs < 1) config = nullptr;
    if (!config && !(d.surfaceless && d.noConfig)) {
        why = "no offscreen drawing: EGL has no configuration for OpenGL";
        return false;
    }
    EGLContext context = e.createContext(d.display, config, nullptr, nullptr);
    if (!context) {
        why = "no offscreen drawing: EGL made no OpenGL context";
        return false;
    }
    // Drawn into a framebuffer object, so a surface is only a formality:
    // none where EGL allows it, one pixel otherwise.
    EGLSurface surface = nullptr;
    if (!d.surfaceless && config) {
        const EGLint size[] = {kWidth, 1, kHeight, 1, kNone};
        surface = e.createPbuffer(d.display, config, size);
    }
    if (!e.makeCurrent(d.display, surface, surface, context)) {
        if (surface) e.destroySurface(d.display, surface);
        e.destroyContext(d.display, context);
        why = "no offscreen drawing: the OpenGL context would not become current";
        return false;
    }

    const double s = std::clamp(std::isfinite(scale) ? scale : 1.0, 0.25, 4.0);
    const int w = std::max(1, static_cast<int>(std::lround(editor.width() * s)));
    const int h = std::max(1, static_cast<int>(std::lround(editor.height() * s)));
    bool ok = false;
    if (const unsigned fb = canvasTargetCreate(w, h, resolveGl)) {
        if (NVGcontext* vg = canvasCreate(nullptr)) {
            if (editor.attach(vg)) {
                editor.idle(0.0);  // telemetry drained: the meters show what played
                canvasViewport(w, h);
                const NVGcolor bg = rgb(editor.background());
                canvasClear(vg, bg.r, bg.g, bg.b);
                nvgBeginFrame(vg, static_cast<float>(w), static_cast<float>(h), 1.0f);
                nvgScale(vg, static_cast<float>(s), static_cast<float>(s));
                editor.render(vg);
                nvgEndFrame(vg);
                std::vector<uint8_t> rows(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
                canvasReadPixelsRGBA(w, h, rows.data());
                // GL's rows run from the bottom.
                out.width = w;
                out.height = h;
                out.rgba.resize(rows.size());
                const size_t stride = static_cast<size_t>(w) * 4;
                for (int y = 0; y < h; ++y)
                    std::memcpy(out.rgba.data() + static_cast<size_t>(y) * stride,
                                rows.data() + static_cast<size_t>(h - 1 - y) * stride, stride);
                ok = true;
                editor.detach();
            } else {
                why = "the editor's fonts would not load";
            }
            canvasDestroy(vg);
        } else {
            why = "NanoVG would not start on the offscreen context";
        }
        canvasTargetDestroy(fb);
    } else {
        why = "no offscreen drawing: the OpenGL context has no framebuffer objects";
    }
    e.makeCurrent(d.display, nullptr, nullptr, nullptr);
    if (surface) e.destroySurface(d.display, surface);
    e.destroyContext(d.display, context);
    return ok;
}

}  // namespace
#endif

bool drawOffscreen(EditorBase& editor, double scale, Picture& out, std::string* why) {
    out = Picture{};
    std::string problem;
#if SUPPLE_OFFSCREEN_EGL
    bool ok = false;
    // A thread of its own: whatever GL context the caller has current stays
    // current, and EGL's per-thread state goes with the thread.
    std::thread worker([&] { ok = draw(editor, scale, out, problem); });
    worker.join();
#else
    (void)editor, (void)scale;
    const bool ok = false;
    problem = "drawing an editor without a window is not available on this platform yet";
#endif
    if (!ok && why) *why = problem;
    return ok;
}

}  // namespace supple::gui
