// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/gui/window.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>

#include <algorithm>
#include <cstring>
#include <tuple>

namespace supple::gui {
namespace {
Error sdlError(std::string code, std::string what) {
    return Error(std::move(code), what + ": " + SDL_GetError(), {},
                 "the GUI needs an X11 display (XWayland) and OpenGL 3.3; "
                 "tests and benchmarks use the offscreen driver");
}
}  // namespace

Result<std::unique_ptr<Platform>> Platform::start(Driver driver) {
    // Plugin editors are X11 children of our windows, so DAW windows are X11
    // windows even on Wayland desktops (ADR-0005 §1).
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver == Driver::X11 ? "x11" : "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) return sdlError("gui.platform", "SDL_Init");
    std::unique_ptr<Platform> p(new Platform(driver));
    p->wakeEvent_ = SDL_RegisterEvents(1);
    if (p->wakeEvent_ == 0) return sdlError("gui.platform", "SDL_RegisterEvents");
    return p;
}

Platform::~Platform() { SDL_Quit(); }

void Platform::wake() const {
    SDL_Event e{};
    e.type = wakeEvent_;
    (void)SDL_PushEvent(&e);
}

Result<std::unique_ptr<Window>> Window::open(const Platform& platform,
                                             const WindowOptions& options) {
    (void)platform;
    if (!options.gl) {
        std::unique_ptr<Window> w(new Window);
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
        if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
        if (options.utility) flags |= SDL_WINDOW_UTILITY;
        w->window_ = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
        if (!w->window_) return sdlError("gui.window", "SDL_CreateWindow");
        if (options.parent && options.parent->window_)
            (void)SDL_SetWindowParent(w->window_, options.parent->window_);
        return w;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    std::unique_ptr<Window> w(new Window);
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (options.utility) flags |= SDL_WINDOW_UTILITY;
    if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
    w->window_ = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
    if (!w->window_) return sdlError("gui.window", "SDL_CreateWindow");
    if (options.parent && options.parent->window_)
        (void)SDL_SetWindowParent(w->window_, options.parent->window_);
    w->context_ = SDL_GL_CreateContext(w->window_);
    if (!w->context_) return sdlError("gui.window", "SDL_GL_CreateContext");
    SDL_GL_MakeCurrent(w->window_, static_cast<SDL_GLContext>(w->context_));
    SDL_GL_SetSwapInterval(1);  // event-driven: frames only when something changed
    w->vg_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!w->vg_) return Error("gui.window", "NanoVG could not create its GL3 renderer");
    return w;
}

Window::~Window() {
    if (vg_) {
        SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
        nvgDeleteGL3(vg_);
    }
    if (context_) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(context_));
    if (window_) SDL_DestroyWindow(window_);
}

std::uint32_t Window::id() const { return SDL_GetWindowID(window_); }

void Window::setSize(int width, int height) { SDL_SetWindowSize(window_, width, height); }

void Window::setPosition(int x, int y) { SDL_SetWindowPosition(window_, x, y); }

std::pair<int, int> Window::position() const {
    int x = 0, y = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    return {x, y};
}

bool onDisplay(int x, int y, int width, int height) {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays) return false;
    bool shown = false;
    for (int i = 0; i < count && !shown; ++i) {
        SDL_Rect b{};
        if (!SDL_GetDisplayBounds(displays[i], &b)) continue;
        const int w = std::min(x + width, b.x + b.w) - std::max(x, b.x);
        const int h = std::min(y + height, b.y + b.h) - std::max(y, b.y);
        shown = w >= std::min(48, width) && h >= std::min(48, height);
    }
    SDL_free(displays);
    return shown;
}

void Window::setResizable(bool resizable) { SDL_SetWindowResizable(window_, resizable); }

void Window::setTitle(const std::string& title) { SDL_SetWindowTitle(window_, title.c_str()); }

float Window::width() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    return static_cast<float>(w);
}

float Window::height() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    return static_cast<float>(h);
}

float Window::pixelRatio() const {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    return w > 0 ? static_cast<float>(pw) / static_cast<float>(w) : 1.0f;
}

unsigned long Window::x11Window() const {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window_);
    return static_cast<unsigned long>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
}

std::string Window::renderer() const {
    makeCurrent();
    const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    return std::string(r ? r : "?") + " | " + (v ? v : "?");
}

void Window::makeCurrent() const {
    SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
}

void Window::forceCurrent() const {
    if (!context_) return;
    SDL_GL_MakeCurrent(window_, nullptr);  // SDL now knows nothing is current
    SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
}

void Window::show() { SDL_ShowWindow(window_); }

void Window::hide() { SDL_HideWindow(window_); }

void Window::beginFrame(std::uint32_t rgb) {
    SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    glViewport(0, 0, pw, ph);
    glClearColor(static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                 static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                 static_cast<float>(rgb & 0xff) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    nvgBeginFrame(vg_, width(), height(), pixelRatio());
}

void Window::endFrame() { nvgEndFrame(vg_); }

void Window::present() { SDL_GL_SwapWindow(window_); }

Image Window::readPixels() const {
    Image image;
    SDL_GetWindowSizeInPixels(window_, &image.width, &image.height);
    const auto row = static_cast<std::size_t>(image.width) * 4;
    std::vector<std::uint8_t> bottomUp(row * static_cast<std::size_t>(image.height));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    image.rgba.resize(bottomUp.size());
    for (int y = 0; y < image.height; ++y)
        std::memcpy(image.rgba.data() + static_cast<std::size_t>(y) * row,
                    bottomUp.data() + static_cast<std::size_t>(image.height - 1 - y) * row, row);
    return image;
}

Result<Fonts> loadFonts(NVGcontext* vg, const std::filesystem::path& dir) {
    Fonts f;
    auto load = [&](const char* name, const char* file) -> Result<int> {
        const auto path = dir / file;
        const int id = nvgCreateFont(vg, name, path.c_str());
        if (id < 0)
            return Error("gui.font", "cannot load font " + path.string(), {},
                         "the bundled fonts live in assets/fonts/");
        return id;
    };
    for (auto [slot, name, file] : {std::tuple{&f.ui, "ui", "Inter-Regular.ttf"},
                                    std::tuple{&f.uiBold, "ui-bold", "Inter-SemiBold.ttf"},
                                    std::tuple{&f.mono, "mono", "PlexMono-Regular.ttf"},
                                    std::tuple{&f.monoBold, "mono-bold", "PlexMono-Medium.ttf"}}) {
        auto id = load(name, file);
        if (!id) return std::move(id).error();
        *slot = *id;
    }
    return f;
}

}  // namespace supple::gui
