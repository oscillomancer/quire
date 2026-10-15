// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "editor_platform.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cerrno>
#include <vector>
namespace supple::host {
Status initializeEditorPlatform() {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
        AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                     DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return {};
    return Error("host.editor.dpi", "cannot initialize bridge DPI awareness", "/editor/scale",
                 "start the bridge before creating any native windows");
}
int waitEditorEvents(std::span<WaitItem> items, int timeoutMs) {
    // Service native input even while a busy control channel stays ready.
    MSG pending;
    for (int n = 0; n < 256 && PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE); ++n) {
        TranslateMessage(&pending);
        DispatchMessageW(&pending);
    }
    const int ready = waitFor(items, 0);
    if (ready != 0) return ready;
    std::vector<HANDLE> handles;
    for (const auto& item : items) {
        if (!item.source.valid() || !item.source.nativeHandle()) {
            errno = EINVAL;
            return -1;
        }
        const auto h = static_cast<HANDLE>(item.source.nativeHandle());
        if (std::find(handles.begin(), handles.end(), h) == handles.end()) handles.push_back(h);
    }
    if (handles.size() >= MAXIMUM_WAIT_OBJECTS) {
        errno = EINVAL;
        return -1;
    }
    const auto result = MsgWaitForMultipleObjectsEx(
        static_cast<DWORD>(handles.size()), handles.data(),
        timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED) {
        errno = EIO;
        return -1;
    }
    // Bound each batch so continuous UI messages cannot starve IPC/timers.
    MSG message;
    for (int n = 0; n < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return waitFor(items, 0);
}
}  // namespace supple::host
