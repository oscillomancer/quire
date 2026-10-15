// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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
// A Cocoa pointer is process-local. Only Floating carries a server window
// number across processes; zero means no owner (standalone host clients).
inline constexpr NativeWindow cocoaBridgeParent(NativeWindow parent) {
    return {NativeWindow::Kind::Floating,
            parent.kind == NativeWindow::Kind::Floating ? parent.handle : 0};
}
inline constexpr bool validFloatingEditorParent(NativeWindow parent) {
    return parent.kind == NativeWindow::Kind::Floating &&
           parent.handle <= std::numeric_limits<std::uint32_t>::max();
}
inline bool validEditorScale(double scale) { return std::isfinite(scale) && scale > 0; }
// A resizable editor keeps its logical size when the scale changes: `width` and
// `height` (pixels at scale `from`) become pixels at scale `to`. Not applied to
// editors that resized themselves in response to the new scale.
inline void rescaleEditorSize(std::uint32_t& width, std::uint32_t& height, double from, double to) {
    if (!validEditorScale(from) || !validEditorScale(to)) return;
    const auto scaled = [&](std::uint32_t v) {
        return static_cast<std::uint32_t>(
            std::max(1.0, std::min(std::round(static_cast<double>(v) * to / from), 1.0e6)));
    };
    width = scaled(width);
    height = scaled(height);
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
    void showAbove(std::uint32_t ownerWindowNumber);
    void hide();
    bool closed() const;
    bool takeResize(unsigned& width, unsigned& height);

  private:
    FloatingEditorWindow();
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace supple::host
