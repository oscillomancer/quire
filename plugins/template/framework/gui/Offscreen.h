// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// An editor drawn without a window or a display, for tools and layers that
// show a person, an agent or a test what it looks like (a layer's
// Instance::drawEditor).
//
// Linux and the BSDs only, for now: through EGL with no surface, which Mesa
// provides on a GPU and in software alike. libEGL is loaded the first time a
// picture is asked for, so a plugin has no link dependency on it. Elsewhere,
// and where EGL is missing, drawOffscreen() says why and draws nothing.
//
// The editor gets a context of its own, on a thread of its own (so a GL
// context the caller has current is left alone), is drained of telemetry
// once (its meters show what played since the last drain), drawn once at
// its design size times `scale`, and let go. It must not be in a window. The
// caller waits: call it from the main thread, as the editor would run.
#pragma once

#include <string>

#include "core/Picture.h"

namespace supple::gui {

class EditorBase;

// False, saying why in `why`, when nothing could be drawn. `scale` is
// clamped to 0.25 .. 4.
bool drawOffscreen(EditorBase& editor, double scale, Picture& out, std::string* why = nullptr);

}  // namespace supple::gui
