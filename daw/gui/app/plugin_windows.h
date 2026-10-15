// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Plugin editors in native windows (docs/specs/gui.md §4.5): X11/NSView/HWND
// embedding, or a bridge-owned floating window on macOS. Their
// timers and file descriptors serviced from the UI thread (= the plugin's main
// thread), its resize requests applied. Replacement retains the local/X11/Win32
// container; a Cocoa bridged replacement opens a fresh child-owned window.
// Removing the effect closes its editor.
#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>
#include "service_watcher.h"

#include "supple/engine/app.h"
#include "supple/gui/window.h"
#include "supple/json/value.h"

union SDL_Event;

namespace supple::gui {

// Editor sizes to SDL window coordinates. Pixel editors must not round through
// integer design units; Cocoa supplies points, already SDL window coordinates.
std::pair<int, int> editorWindowSize(std::uint32_t width, std::uint32_t height,
                                     NativeWindow::Kind kind, double pixelRatio, double inputScale);

// Whether an editor window's display scale is no longer the one its plug-in was
// last given (`given`), so the plug-in must be told. Ignores float noise.
bool editorScaleChanged(double given, double current);

// Where to open a bridge-owned (Cocoa) editor window: the remembered placement
// when it is still on a display, with its size only when one was remembered
// (a resizable editor). Empty: let the bridge place it.
std::optional<host::Instance::EditorFrame> restoredEditorFrame(int x, int y, int width, int height);

// Applies coalesced host requests without destroying a hidden editor. Closed
// editors are removed by the owner; close takes precedence over other flags.
void applyEditorRequests(Window& window, const host::Instance::EditorRequests& requests);

class PluginWindows {
  public:
    PluginWindows(const Platform& platform, engine::EngineApp& app, std::function<void()> wake);
    ~PluginWindows();

    // Opens (or raises) the editor of an effect in the session.
    Status open(model::Id fx);
    // Opens an editor for an instance the caller owns (tests, previews).
    Status open(host::Instance& instance, const std::string& title);
    void close(model::Id fx);
    // Opens the effect's editor, or closes it when it is open (the FX chain's
    // "UI" and the mixer's slot buttons).
    Status toggle(model::Id fx);
    void closeAll();
    // Re-order Cocoa bridged editors above the owner when the DAW regains focus.
    void appActivated();
    // The app's window: editors float above it, and its GL context is made
    // current again after plugin code ran (editors draw with their own).
    void setOwner(const Window* owner) { owner_ = owner; }
    std::size_t count() const { return editors_.size(); }
    bool isOpen(model::Id fx) const;
    bool hasFocus(model::Id fx) const;  // its editor's window has the input focus
    // True once after an open editor's remembered placement changed without an
    // input event (a bridge-owned window moved): the UI file should be checked.
    bool takeLayoutChanged() { return std::exchange(layoutChanged_, false); }
    std::function<void(const Error&)> onError;

    // Where each effect's editor was last: its position and, when the editor
    // is resizable, its size. An editor opens there again; the project's UI
    // file keeps them ({fx id: {x, y, width, height}}).
    json::Value saveLayout() const;
    void loadLayout(const json::Value& layout);

    // Events addressed to editor windows; true when consumed.
    bool handle(const SDL_Event& e);
    // Applies editor requests and rebuilds the shared watch set. App instance
    // timers run in EngineApp::tick; caller-owned local editors add their own
    // timer deadlines here. Returns their nearest delay (-1: none).
    int service();
    // Shared watcher: dispatch the previous pass's app/editor readiness, then
    // suspend before model commands can close/reuse descriptors. service()
    // rebuilds the set afterwards, even when no editor is open.
    bool dispatchFds();
    void suspendFdWatches();
    void setModelWake(Waitable source) { modelWake_ = source; }
    void setModelWakeFd(int fd) { setModelWake(Waitable(fd)); }
    std::optional<Error> fdError() { return watcher_ ? watcher_->error() : watcherError_; }

  private:
    struct Placement {
        int x = 0, y = 0;
        int width = 0, height = 0;  // 0: the editor's own size (not resizable)
        bool operator==(const Placement&) const = default;
    };
    struct Editor {
        std::optional<model::Id> fx;  // empty: an instance the caller owns
        host::Instance* instance = nullptr;
        std::uint64_t serial = 0;  // this registration of the instance's descriptors
        std::unique_ptr<Window> window;
        bool resizable = false;
        double scale = 1;                // the display scale its plug-in was last given
        std::optional<Placement> frame;  // a bridge-owned window's last reported place
    };
    static std::optional<Placement> placementOf(const Editor& editor);
    // A bridge-owned window's reported frame becomes the effect's placement.
    void noteFrame(Editor& editor, const host::Instance::EditorFrame& frame);
    void remember(const Editor& editor);
    void place(Editor& editor);  // before the window is shown
    NativeWindow floatingParent() const;
    Status attach(Editor editor, const std::string& title);
    // The window's display scale changed (monitor move, system scale): tell the
    // plug-in and give the window the size it answers with. True when it did.
    bool rescale(Editor& editor);
    bool reattach(Editor& editor);  // the effect's current instance, in the same window
    void drop(std::size_t index);
    void restoreContext() const {
        if (owner_) owner_->forceCurrent();
    }
    const Platform& platform_;
    const Window* owner_ = nullptr;
    engine::EngineApp& app_;
    std::vector<Editor> editors_;
    std::uint64_t nextSerial_ = 1;
    std::uint64_t nextServiceSerial_ = 4;  // even; editors have odd serials
    struct ServiceWatch {
        engine::EngineApp::ServiceFd source;
        std::uint64_t serial;
    };
    std::vector<ServiceWatch> serviceWatches_;
    Waitable modelWake_;
    std::map<model::Id, Placement> placements_;
    std::unique_ptr<FdWatcher> watcher_;
    std::optional<Error> watcherError_;
    bool watcherErrorReported_ = false;
    bool layoutChanged_ = false;
};

}  // namespace supple::gui
