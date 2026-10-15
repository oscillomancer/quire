// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <memory>
#include <span>
#include <string_view>
#include "supple/base/native_window.h"
#include "supple/base/result.h"
#include "supple/base/waitable.h"
namespace supple::host {
inline constexpr const char* editorClapApi() {
#if defined(__APPLE__)
    return "cocoa";
#elif defined(_WIN32)
    return "win32";
#else
    return "x11";
#endif
}
inline constexpr const char* editorVst3Api() {
#if defined(__APPLE__)
    return "NSView";
#elif defined(_WIN32)
    return "HWND";
#else
    return "X11EmbedWindowID";
#endif
}
// Called by the bridge before loading modules (Windows DPI awareness).
Status initializeEditorPlatform();
// Bridge main-thread wait: native UI input and IPC share one event loop.
int waitEditorEvents(std::span<WaitItem> items, int timeoutMs);
// macOS bridge-owned NSWindow. Other platforms embed into the parent's window.
class FloatingEditorWindow {
  public:
    static Result<std::unique_ptr<FloatingEditorWindow>> create(std::string_view title);
    ~FloatingEditorWindow();
    NativeWindow parent() const;
    void resize(unsigned width, unsigned height, bool resizable);
    void show();
    void hide();
    bool closed() const;
    bool takeResize(unsigned& width, unsigned& height);

  private:
    FloatingEditorWindow();
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace supple::host
