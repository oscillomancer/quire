// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// An editor drawn without a window or a display, for tools and layers that
// show a person, an agent or a test what it looks like (a layer's
// Instance::drawEditor).
//
// Linux and the BSDs use EGL without a surface (Mesa supports GPUs and
// software rendering); libEGL is loaded on demand, with no link dependency.
// Windows uses WGL on a hidden window, requiring OpenGL framebuffer objects.
// macOS uses headless Metal with the same renderer as the plugin (or CGL if
// configured without Metal). Missing devices/drivers fail with an explanation.
//
// The editor gets a context of its own, on a thread of its own (so a GL
// context the caller has current is left alone), is drained of telemetry
// once (its meters show what played since the last drain), takes the
// pointer input given, in order, as from a window, is drawn once at its
// design size times `scale`, and is let go: it loses focus, which ends
// every gesture the input opened. It must not be in a window. Call it from
// the main thread, which waits while the editor runs on the other: nothing
// else runs beside the editor, but what its edits call reaches the host from
// that thread (CLAP's request_flush and request_restart may come from any;
// mark_dirty and the note-name change may not). So it is for a plugin's own
// tools, such as an extension its host knows, not for a host's editor.
#pragma once

#include <span>
#include <string>

#include "core/Picture.h"

namespace supple::gui {

class EditorBase;

// False, saying why in `why`, when no picture could be drawn. Pointer input
// is applied only after context and font setup succeed; a later readback
// failure can still leave edits applied. `scale` is clamped to 0.25 .. 4.
bool drawOffscreen(EditorBase& editor, double scale, std::span<const PointerInput> input, Picture& out,
                   std::string* why = nullptr);
inline bool drawOffscreen(EditorBase& editor, double scale, Picture& out, std::string* why = nullptr) {
    return drawOffscreen(editor, scale, {}, out, why);
}

}  // namespace supple::gui
