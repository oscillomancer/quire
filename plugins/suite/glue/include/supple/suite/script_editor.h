// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The editor of a script that draws its own (script.h's editor() and
// draw()), in the framework's GUI stack: the player's, and a source
// export's. The script layer makes it (ScriptLayer::createEditor).
//
//   - It runs the script's editor code in an instance of the plugin struct
//     of its own, on the main thread: before each hook, every Param takes
//     the value the DSP is using (with the host's modulation; a meter() what
//     the processor last reported) and the Shared members what the audio
//     code last sent (SharedChannel), so draw() sees what is heard.
//   - editor(e) runs once, as it opens: its size and controls, the
//     framework's own (Ui), bound to the script's parameters and files.
//     draw(g) runs at every repaint, under the controls, through a Canvas
//     over NanoVG; colours named by Theme:: come from the product's palette.
//   - pointer(p) hears presses and the wheel where no control is, every
//     move, the drags and release of a press it got, and the pointer
//     leaving. Its edits (Param::edit) go to the host through Ui, on hidden
//     controls, so they keep Ui's gesture rules: a press's are one gesture
//     until its release (two parameters to a gesture), a wheel's a burst.
//   - It repaints when a parameter moves, when new Shared values differ from
//     those drawn, and as the controls ask; an idle editor draws nothing.
//   - A hook that loops past its budget is stopped, and the editor shows
//     that instead of drawing; an index out of range is reported once. Both
//     go to the host's log with the line.
#pragma once

#include <memory>

#include "gui/EditorBase.h"
#include "supple/script_code.h"
#include "supple/suite/script_shared.h"

namespace supple::suite {

// The editor for `code`, whose Shared members arrive by `channel` (null when
// it has none); null when the script draws nothing of its own, which then
// has the automatic editor.
std::unique_ptr<gui::EditorBase> makeScriptEditor(gui::EditorHost& host,
                                                  std::shared_ptr<const script::detail::Code> code,
                                                  std::shared_ptr<SharedChannel> channel);

}  // namespace supple::suite
