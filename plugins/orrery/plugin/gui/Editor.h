// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Orrery's editor: the patch as a signal-flow diagram (oscillators → filters →
// drive → amp → fx → out) and a modulator strip on the left; the selected
// module's parameters and the modulation routes on the right. Everything it
// shows comes from the EditorBridge — the structure decides which modules and
// parameters exist — and every structural change it makes is a semantic merge
// patch (orrery/edits.h), so the editor cannot build a patch the codec rejects.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nanovg.h>

#include "Theme.h"
#include "Window.h"
#include "orrery/editor_bridge.h"

namespace orrery::gui {

using EditorHost = supple::orrery::EditorBridge;

enum class EditorKey { Tab, Left, Right, Up, Down, Home, End, Enter, Space };

class Editor {
  public:
    explicit Editor(EditorHost& host);
    bool attach(NVGcontext* vg);
    void onContextLost() { fontsReady_ = false; }
    void pump(double elapsedSeconds);
    void render(NVGcontext* vg);
    bool wantsRepaint() const;
    void markDirty() { dirty_ = true; }
    void setVisible(bool on);

    // Basic input (the preview tool and simple hosts).
    bool onMouseDown(float x, float y) { return onPointerDown(x, y, 0, 0); }
    void onMouseMove(float x, float y, bool fine) { onPointerMove(x, y, fine ? kModShift : 0); }
    void onMouseUp() { onPointerUp(0, 0, 0, 0); }
    void onDoubleClick(float x, float y) { onPointerDouble(x, y, 0, 0); }
    bool onScroll(float x, float y, float dy) { return onWheel(x, y, 0, dy, 0); }
    void onKey(EditorKey key, bool fineOrReverse);
    void onFocusLost();
    bool dragging() const { return drag_.kind != Drag::None; }

    // Extended input (Window.h).
    bool onPointerDown(float x, float y, int button, uint32_t mods);
    void onPointerDouble(float x, float y, int button, uint32_t mods);
    void onPointerUp(float x, float y, int button, uint32_t mods);
    void onPointerMove(float x, float y, uint32_t mods);
    bool onWheel(float x, float y, float dx, float dy, uint32_t mods);
    bool onKeyCode(uint32_t key, uint32_t mods);
    void onPointerLeave();
    Cursor cursor() const;

    // For tests: what is on screen, and the message line.
    struct Hit {
        enum class Kind : uint8_t {
            None,
            Tile,        // a = slot
            AddTile,     // a = slot (absent, addable)
            Knob,        // a = param index
            Choice,      // a = param index (enum/bool: opens a menu)
            TypePill,    // a = slot
            TargetPill,  // a = oscillator 0..3
            Remove,      // a = slot
            Routing,
            Quality,      // a = quality index
            RouteAmount,  // a = route index
            RouteRemove,  // a = route index
            AddRoute,
            AddFx,
            Macro,     // a = param index of the macro value
            MenuItem,  // a = item index
        };
        Kind kind = Kind::None;
        float x = 0, y = 0, w = 0, h = 0;
        int a = -1;
        bool contains(float px, float py) const {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };
    const std::vector<Hit>& hits() const { return hits_; }
    const std::string& status() const { return status_; }
    supple::orrery::Slot selected() const { return selected_; }
    bool menuOpen() const { return menu_.open; }
    const std::vector<std::string>& menuItems() const { return menu_.items; }
    void layout();  // rebuilds hits() from the bridge (render calls it)

  private:
    struct Drag {
        enum Kind : uint8_t { None, Param, RouteAmount } kind = None;
        int target = -1;
        float startY = 0.0f;
        double startValue = 0.0;  // normalized (param) or the amount (route)
    };
    struct Menu {
        bool open = false;
        float x = 0, y = 0, w = 180;
        std::string title;
        std::vector<std::string> items;
        std::function<void(int)> pick;
        int hover = -1;
    };

    const Hit* hitAt(float x, float y) const;
    void openMenu(float x, float y, std::string title, std::vector<std::string> items,
                  std::function<void(int)> pick);
    void closeMenu();
    void apply(const supple::json::Value& patch);  // a structural merge; errors → status
    void select(supple::orrery::Slot s);
    void setParamNormalized(std::size_t index, double normal);
    void editOnce(std::size_t index, double physical);
    void closeWheelGesture();
    void openChoiceMenu(const Hit& h);
    void openTypeMenu(const Hit& h);
    void openAddRouteMenu(float x, float y);
    std::vector<std::size_t> inspectorParams() const;

    void drawHeader(NVGcontext* vg);
    void drawFlow(NVGcontext* vg);
    void drawModulators(NVGcontext* vg);
    void drawInspector(NVGcontext* vg);
    void drawRoutes(NVGcontext* vg);
    void drawMenu(NVGcontext* vg);
    void drawTile(NVGcontext* vg, const Hit& h, const char* title, const char* detail,
                  unsigned tone);
    void drawKnob(NVGcontext* vg, const Hit& h, std::size_t index);

    EditorHost& host_;
    std::vector<Hit> hits_;
    Menu menu_;
    Drag drag_;
    supple::orrery::Slot selected_ = supple::orrery::kOsc1;
    int hover_ = -1;  // index into hits_
    int focus_ = -1;  // param index with keyboard focus
    int wheelParam_ = -1;
    float wheelIdle_ = 0.0f;
    std::string status_;
    bool statusError_ = false;
    float statusAge_ = 0.0f;
    std::string describe_;
    std::uint64_t seenRevision_ = ~0ull;
    float peakL_ = 0.0f, peakR_ = 0.0f;
    int voices_ = 0;
    bool fontsReady_ = false;
    mutable bool dirty_ = true;
};

}  // namespace orrery::gui
