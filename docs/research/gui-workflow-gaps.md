# GUI workflow-gap log against `reaper-ux.md` §16 (P5.17)

Living document: each of REAPER's top-40 workflows (the research's
priority list) against what Quire's GUI does today. "Done" means a person can
do it in the GUI, through commands, with one undo step per gesture. Updated
2026-10-01 (orchestrator) at p5-widgets `9ba99f5` plus work in flight.

Status: **done** · **partial** (what is missing) · **in progress** (owner,
branch) · **gap** (not started).

## P0 (1–20)

| # | Workflow | Status |
|---|---|---|
| 1 | Create a track instantly, rename in place | partial — Ctrl+T / Track menu create; inline rename in the TCP in progress (`gui`, p5-tcp) |
| 2 | Turn a track into a folder/bus, show parentage | partial — `track.set {type}`, `track.move`; the TCP's parentage bar in progress (`gui`) |
| 3 | Drag a track output to another track to send | partial — one click in the routing matrix creates the send; no drag from a TCP output yet |
| 4 | Inspect/edit each send (source, destination, channels, tap) | partial — matrix details row: tap, level, pan, mute, remove; channel maps are not editable (`route.*` has no channels argument) |
| 5 | Routing overview with legible cycles/sidechains | done — routing matrix: blocked cells say why (cycle path, strip rules), keys as destinations; a graph view is P1 |
| 6 | Track channel count, routes to 3/4 and beyond | gap (by design so far) — strips are mono/stereo (session model); multichannel is a model change |
| 7 | Parent/master send independent of explicit sends | done here — `track.set {parent_send}` and a toggle in the routing matrix (the outlined parent cell) |
| 8 | Drop audio/MIDI files onto tracks and timeline | partial — File > Import (one transaction); drag-and-drop in progress (`gui2`, p5-edit) |
| 9 | Move, trim, split, duplicate with direct gestures | in progress (`gui2`, p5-edit); the commands exist |
| 10 | Draw fades, change crossfades | in progress (`gui2`, p5-edit) |
| 11 | Slip media inside an item | in progress (`gui2`, p5-edit) |
| 12 | Stretch with rate/pitch mode visible | gap — `take.set {playrate, pitch, preserve_pitch}` exists; no gesture yet (next after p5-edit) |
| 13 | Switch takes, see the active take | partial — take-count badges drawn; click-to-cycle in progress (`gui2`) |
| 14 | Searchable command list from anywhere | done — actions list (Shift+/), every command + built-ins + macros |
| 15 | Assign shortcuts, show conflicts by context | done — Ctrl+B in the actions list, keymap sections, user deltas |
| 16 | Add, reorder, bypass, wet/dry FX quickly | done — FX chain window + plugin browser (favourites, stock first) + parameter panel |
| 17 | Render master with clear source/bounds/format/destination | done — render dialog with live dry-run file names, jobs, add-to-project |
| 18 | Arm, pick input, monitor, record without hidden state | partial — arm/monitor in the mixer; TCP + transport record in progress (`gui`); **no input picker** (hardware inputs are not listed by any command) and the record path is not shown before recording |
| 19 | Save/reopen with stable IDs and readable diffs | done — File new/open/save/save as, canonical JSON, confirm before discarding |
| 20 | Undo/redo every edit, expose command results | done — history dock (versions, branches, jump), command log (`log.list`) with origins |

## P1 (21–36)

| # | Workflow | Status |
|---|---|---|
| 21 | Take lanes, fixed-lane comping | gap |
| 22 | Take FX, item/take envelopes | partial — commands (`env.*` on takes, take chains in the model); no UI |
| 23 | Tempo ramps and meter changes on the timeline | partial — drawn in the ruler; edited only through `tempo.set` |
| 24 | Markers/regions: create, rename, navigate | partial — drawn; `marker.*`/`region.*` commands; no create/rename gestures |
| 25 | Piano roll, velocity and CC lanes | partial — MIDI editor with velocity lane and notation panel; CC lane UI missing (`midi.edit` cc ops exist) |
| 26 | MIDI event list and notation views | partial — notation panel done; event list missing |
| 27 | Track automation lanes, read/write modes | gap (UI) — `env.*` commands exist and the engine plays them (P5.14, after p5-edit) |
| 28 | Automation items with pooled identity | gap — modelled (`ai_*`), no commands |
| 29 | Project/track/item timebase with visible consequences | gap (UI) |
| 30 | Track templates and FX chain presets | gap |
| 31 | FX pin maps, detector input on 3/4 | gap — key routes (ADR-0018) cover the common sidechain case |
| 32 | Render selected tracks/items/regions as stems | done — render dialog stems/regions |
| 33 | Queue exports, second output format | partial — renders are jobs; one format per render |
| 34 | True-peak/LUFS normalization, render measurements | done — normalize/target/limit in the dialog, loudness in results |
| 35 | Loop record into takes, punch modes | partial — engine done (ADR-0014/15); transport modes in progress (`gui`) |
| 36 | Peaks cached, never blocking editing | done — worker-built pyramids, disk cache |

## P2 (37–40)

| # | Workflow | Status |
|---|---|---|
| 37 | Contextual mouse gestures, toolbar/menu customization | partial — menus.json + keymap; mouse modifiers fixed |
| 38 | Screensets and track layouts | partial — per-project UI state (panels, zoom, selection); no named screensets |
| 39 | Isolated plug-ins with visible recovery state | partial — isolation in the engine; failures reach the status line; no per-FX state badge; isolated/VST3-bridged editors not embeddable yet |
| 40 | Reusable multi-action macros with typed inputs and undo | done — macros as one undo step, prompted inputs |

## Gaps found by this pass, and where they go

- Parent-send toggle (#7): fixed in this pass.
- Hardware input listing for an input picker (#18): engine — a command that
  lists the backend's capture ports/channels; then the TCP's input menu.
- Route channel maps (#4): engine — `route.add/set {channels}` over the
  existing `ChannelMap`.
- Stretch gesture (#12) and take-lane comping (#21): after p5-edit.
- Automation lanes (#27): P5.14, after p5-edit.
- CC lane (#25), event list (#26), marker/region and tempo gestures (#23, #24):
  queued for the MIDI editor and arrange follow-ups.
