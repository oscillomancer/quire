// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstdint>
namespace supple {
// Borrowed UI-thread parent. Cocoa means NSView*, Win32 means HWND. Never
// dereference another process's pointer. Floating asks the bridge to own a window;
// its handle is the Cocoa owner's server windowNumber (uint32), or zero if absent.
struct NativeWindow {
    enum class Kind : std::uint8_t { Unavailable, X11, Cocoa, Win32, Floating };
    Kind kind = Kind::Unavailable;
    std::uintptr_t handle = 0;
    bool valid() const {
        return kind == Kind::Floating || (kind != Kind::Unavailable && handle != 0);
    }
};
inline constexpr NativeWindow::Kind nativeWindowKind() {
#if defined(__APPLE__)
    return NativeWindow::Kind::Cocoa;
#elif defined(_WIN32)
    return NativeWindow::Kind::Win32;
#else
    return NativeWindow::Kind::X11;
#endif
}
}  // namespace supple
