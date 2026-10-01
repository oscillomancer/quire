// A DAW window: SDL3 (X11 driver, or the offscreen driver for tests and
// benchmarks), one OpenGL 3.3 core context, NanoVG GL3 for chrome (ADR-0005).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "supple/base/result.h"

struct NVGcontext;
struct SDL_Window;

namespace supple::gui {

// Video subsystem lifetime. One per process, before any Window.
class Platform {
  public:
    enum class Driver { X11, Offscreen };
    static Result<std::unique_ptr<Platform>> start(Driver driver);
    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Driver driver() const { return driver_; }
    // An SDL event type that other threads may push to wake the event loop.
    std::uint32_t wakeEvent() const { return wakeEvent_; }
    // Thread-safe: interrupts SDL_WaitEvent* on the UI thread.
    void wake() const;

  private:
    explicit Platform(Driver d) : driver_(d) {}
    Driver driver_;
    std::uint32_t wakeEvent_ = 0;
};

struct WindowOptions {
    std::string title;
    int width = 1280, height = 800;  // design units
    bool hidden = false;
};

// RGBA8, rows top to bottom.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

struct Fonts {
    int ui = -1, uiBold = -1, mono = -1, monoBold = -1;
};

class Window {
  public:
    static Result<std::unique_ptr<Window>> open(const Platform& platform,
                                                const WindowOptions& options);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    NVGcontext* vg() const { return vg_; }
    SDL_Window* sdl() const { return window_; }
    std::uint32_t id() const;
    // Size in design units and the design-unit → pixel factor.
    float width() const;
    float height() const;
    float pixelRatio() const;
    // The X11 window id plugin editors are parented to (0 offscreen).
    unsigned long x11Window() const;

    // Makes the context current, clears to `rgb` (0xRRGGBB) and starts a
    // NanoVG frame in design units.
    void beginFrame(std::uint32_t rgb);
    void endFrame();  // flushes NanoVG's draw calls
    void present();   // swaps buffers
    // The back buffer as drawn (between endFrame and present).
    Image readPixels() const;

  private:
    Window() = default;
    SDL_Window* window_ = nullptr;
    void* context_ = nullptr;  // SDL_GLContext
    NVGcontext* vg_ = nullptr;
};

// Loads the bundled fonts (Inter, IBM Plex Mono) from `dir` into `vg`.
Result<Fonts> loadFonts(NVGcontext* vg, const std::filesystem::path& dir);

}  // namespace supple::gui
