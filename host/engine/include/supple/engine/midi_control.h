// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI learn (ADR-0027): what a controller's message is and what it says.
// Shared by the engine (learned plug-in parameters) and the window app
// (actions bound to a controller, the learn dialog). Pure functions and
// small state, no I/O.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "supple/engine/realtime.h"
#include "supple/model/objects.h"

namespace supple::engine {

// The control a message comes from (a note off names its note too); none
// for anything else. `device` is the input port's name.
std::optional<model::MidiControl> controlOf(const ControlMidiEvent& e, std::string_view device);
// Whether the message comes from `c` (its device a case-insensitive
// substring of `device`; an empty one matches any).
bool matches(const model::MidiControl& c, const ControlMidiEvent& e, std::string_view device);

// One message read in a mode.
struct ControlReading {
    double value = 0;       // the control's position, 0..1 (a note off: 0)
    int steps = 0;          // relative modes (a CC): signed encoder steps
    bool relative = false;  // `steps` is what counts
    bool press = false;     // a button pressed: a note on, a program change, a CC above
                            // 0, a bend above the centre
};
ControlReading read(const ControlMidiEvent& e, model::ControlMode mode);

// "CC 21 · ch 1", "Note C3 · ch 10", "Pitch bend · ch 2", "Program 5 · ch 1".
std::string describe(const model::MidiControl& c);
std::string_view modeName(model::ControlMode mode);   // "absolute", "relative1", …
std::string_view modeLabel(model::ControlMode mode);  // "Relative 1 (127 = -1, 1 = +1)"

// Soft takeover's memory, per binding.
struct Takeover {
    std::optional<double> knob;  // where the control was last
    std::optional<double> set;   // the value it last gave the parameter
};

// The parameter's next normalized value from `current` (0..1), or none to
// leave it. `step` is one encoder step (a stepped parameter: one choice).
// Absolute with soft takeover follows only once the control reaches the
// value (within a step, or crossing it), or while the parameter is still
// where the control put it. The caller stores the value it really set in
// `state.set` (a stepped parameter rounds).
std::optional<double> nextValue(double current, const ControlReading& r, model::ControlMode mode,
                                bool softTakeover, double step, Takeover& state);

}  // namespace supple::engine
