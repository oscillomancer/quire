#pragma once
#include <nanovg.h>
#include <array>
#include <cstdint>
#include "../src/EqParams.h"
#include "Telemetry.h"
#include "Theme.h"
namespace eq::gui {
enum class EditorKey { Tab, Left, Right, Up, Down, Home, End, Enter, Space };
class Editor {
  public:
    explicit Editor(EditorHost& host) : host_(host) {
        spectrumPre_.fill(-120);
        spectrumPost_.fill(-120);
    }
    bool attach(NVGcontext*);
    void onContextLost() { fontsReady_ = false; }
    void pump(double elapsedSeconds);
    void render(NVGcontext*);
    bool wantsRepaint() const { return dirty_ || scrollId_ >= 0 || host_.telemetry().pending(); }
    void markDirty() { dirty_ = true; }
    bool onMouseDown(float, float);
    void onMouseMove(float, float, bool);
    void onMouseUp();
    void onPointerLeave() { onMouseUp(); }
    void onDoubleClick(float, float);
    bool onScroll(float, float, float);
    void onKey(EditorKey, bool);
    void onFocusLost();
    void onPointerDown(float x, float y, int button, uint32_t mods);
    void onPointerUp(float, float, int, uint32_t) { onMouseUp(); }
    void onPointerDouble(float x, float y, int button, uint32_t) {
        if (button == 0) onDoubleClick(x, y);
    }
    bool dragging() const { return dragId_ >= 0; }

  private:
    int controlAt(float, float) const;
    int handleAt(float, float) const;
    void closeScroll();
    void setNormal(uint32_t, double);
    EditorHost& host_;
    bool fontsReady_ = false;
    mutable bool dirty_ = true;
    bool zoom_ = false, graphDrag_ = false;
    int dragId_ = -1, focus_ = -1, scrollId_ = -1, menuBand_ = -1;
    float dragY_ = 0, scrollAge_ = 0;
    double dragNormal_ = 0;
    std::array<double, 59> shown_{};
    std::array<float, 4096> pre_{}, preRight_{}, post_{}, postRight_{};
    std::size_t spectrumFill_ = 0;
    std::array<double, 31> spectrumPre_{}, spectrumPost_{};
};
}  // namespace eq::gui
