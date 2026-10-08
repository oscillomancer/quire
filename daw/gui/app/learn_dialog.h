// The MIDI learn dialog (gui.md §4.5, §6; ADR-0027): "Move the MIDI
// controller you wish to assign". It shows the control it heard last, its
// CC mode and options; OK binds it, Remove unbinds what was bound. Opened
// from the actions list (Ctrl+L) and from a parameter's "MIDI" button in the
// FX window. While it is open the devices' messages come to it, and neither
// actions nor learned parameters move.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "app_actions.h"
#include "supple/model/objects.h"
#include "supple/ui/root.h"

namespace supple::gui {

struct LearnChoice {
    model::MidiControl control;
    model::ControlMode mode = model::ControlMode::Absolute;
    bool softTakeover = false, whenSelected = false, whenFocused = false, whenVisible = false;
};

struct LearnRequest {
    std::string title;                   // what is bound: an action's or a parameter's name
    std::optional<LearnChoice> current;  // its binding now: Remove is offered
    bool param = false;                  // a plug-in parameter: soft takeover, enable only when …
    std::function<void(const LearnChoice&)> onOk;
    std::function<void()> onRemove;
};

void openLearnDialog(ui::Root& root, AppActions& actions, engine::EngineApp& app,
                     LearnRequest request);

}  // namespace supple::gui
