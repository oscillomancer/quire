# ADR-0046: Live scalar playrate controls

**Status:** Accepted, 2026-10-17.

Amends ADR-0045 decisions 5 and 10. Contracts: `specs/engine.md`.

## Context

Preparing a complete source/graph replacement for every knob move makes its
control-thread cost grow with track count. Boundaries measured against consumed
audio can already be behind the anticipative renderer when publication finishes.
Recovery then seeks, resets converter/FX state and interrupts otherwise continuous
audio. Repeated preparation and transition coalescing can prevent a drag from
becoming audible until the gesture ends.

## Decision

An edit changing only the saved scalar, with the same pitch mode and unity
bypass state, publishes a small scheduled control transaction to the existing
plan. Persistent session collection identity identifies these edits without
walking tracks. It does not compile the graph, recreate plugins, prime sources,
change the transport epoch or replace converters.

Both executor timelines receive the same device boundary and value through
bounded SPSC queues with a shared release/acquire publication gate. Their sole
owners apply the control on the 64-frame clock grid. A linear ramp runs from
the current ramp value over 1.5 times the audio played since the previous
scalar edit, between one device block (at least 10 ms) and 300 ms, and the
scalar follows those ramps through a one-pole low-pass with a 150 ms time
constant. Fixed 10 ms ramps turned a knob's spaced steps into audible jumps
and holds; ramps measured from played audio (which advances with the device,
unlike the render frontier) join them, and the low-pass removes the remaining
corners where one ramp meets the next, so rate and slope are both continuous.
Smoothness takes priority over response: the knob is heard as a glide, never
as steps. The filter state is part of the clock state, so checkpoints and
structural replay reproduce it exactly. Existing source trajectories and stretch history continue.
The boundary follows both timelines' already generated windows, including source
and PDC lookahead, plus publication headroom. Automation remains authoritative;
scalar controls do not smooth authored envelope steps.

A control-thread history retains changes ahead of the consumed clock snapshot.
Structural preparation replays these controls when predicting the old trajectory,
so an unrelated graph edit does not revert or skip a scheduled scalar ramp.
Queue saturation coalesces requests on the control thread and service retries
the latest value; ordinary 60–100 Hz gestures do not wait for earlier ramps.

Mode changes, unity bypass crossings and structural edits retain the prepared
plan path. Choose and validate that path's handoff against the actual renderer
frontier, rounded to the canonical grid, with headroom for render block size and
observed preparation duration. Retry a missed boundary farther ahead. Keep
prepared-locate recovery for exceptional lateness/history exhaustion.

## Consequences

The saved scalar still governs deterministic offline rendering. Live controls
have device-time scheduling; arbitrary UI gesture timing is not an offline null
promise. Both executors use identical committed control boundaries and the same
clock/conversion implementation. No audio executor allocates or waits to apply
a control. Audible response includes render-ahead and clock/source lookahead;
requested settings and undo update immediately.

Only scalar-only edits qualify. Automation writing that also edits envelope
points and converter configuration changes retain graph preparation. Native
macOS/Windows CI and independent review remain coordinator integration gates.
