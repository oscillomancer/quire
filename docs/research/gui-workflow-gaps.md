# GUI workflow-gap log against `reaper-ux.md` §16 (P5.17)

Living document: each of REAPER's top-40 workflows (the research's
priority list) against what Quire's GUI does today. "Done" means a person can
do it in the GUI, through commands, with one undo step per gesture. Updated
2026-10-01 (orchestrator) at p5-widgets `fc02d3c` plus work in flight.

Status: **done** · **partial** (what is missing) · **in progress** (owner,
branch) · **gap** (not started).

## P0 (1–20)

| # | Workflow | Status |
|---|---|---|
| 1 | Create a track instantly, rename in place | done — Ctrl+T / Track menu / TCP "+ strip"; inline rename in the TCP |
| 2 | Turn a track into a folder/bus, show parentage | done — `track.set {type}`, `track.move`; TCP indentation, type badge and parentage |
| 3 | Drag a track output to another track to send | done — drag a strip's "out" onto another strip (or its lanes); a click opens the routing matrix at the button |
| 4 | Inspect/edit each send (source, destination, channels, tap) | partial — matrix details row: tap, level, pan, mute, remove; channel maps are not editable (`route.*` has no channels argument) |
| 5 | Routing overview with legible cycles/sidechains | done — routing matrix: blocked cells say why (cycle path, strip rules), keys as destinations; a graph view is P1 |
| 6 | Track channel count, routes to 3/4 and beyond | gap (by design so far) — strips are mono/stereo (session model); multichannel is a model change |
| 7 | Parent/master send independent of explicit sends | done here — `track.set {parent_send}` and a toggle in the routing matrix (the outlined parent cell) |
| 8 | Drop audio/MIDI files onto tracks and timeline | done — File > Import and drag-and-drop onto the lanes (first file on the channel under the pointer, the rest on new tracks; one transaction) |
| 9 | Move, trim, split, duplicate with direct gestures | done — move (snapped, across channels), trim (left edge keeps the material), Ctrl-drag copy, S splits at the playhead, Ctrl+C/X/V and Ctrl+D (paste reproduces the items as copied, even after a cut), Ctrl+A; one undo step each |
| 10 | Draw fades, change crossfades | done — fade handles (Ctrl+click cycles the shape); overlaps made by a move, copy or trim crossfade automatically |
| 11 | Slip media inside an item | done — Alt-drag |
| 12 | Stretch with rate/pitch mode visible | done — Alt on an item's right edge sets length and play rate together; the label shows the rate and "varispeed" when pitch is not preserved |
| 13 | Switch takes, see the active take | done — the take badge cycles the active take |
| 14 | Searchable command list from anywhere | done — actions list (Shift+/), every command + built-ins + macros |
| 15 | Assign shortcuts, show conflicts by context | done — Ctrl+B in the actions list, keymap sections, user deltas |
| 16 | Add, reorder, bypass, wet/dry FX quickly | done — FX chain window + plugin browser (favourites, stock first) + parameter panel |
| 17 | Render master with clear source/bounds/format/destination | done — render dialog with live dry-run file names, jobs, add-to-project |
| 18 | Arm, pick input, monitor, record without hidden state | done — arm/monitor/input per strip (the picker names the device's ports from `engine.inputs`), record/loop/punch on the transport bar, a metronome (live output only). Count-in and showing the record path before recording are open |
| 19 | Save/reopen with stable IDs and readable diffs | done — File new/open/save/save as, canonical JSON, confirm before discarding |
| 20 | Undo/redo every edit, expose command results | done — history dock (versions, branches, jump), command log (`log.list`) with origins |

## P1 (21–36)

| # | Workflow | Status |
|---|---|---|
| 21 | Take lanes, fixed-lane comping | done — an item with several takes shows them as lanes; click a lane to hear that take, swipe a range to comp it in (split + take, one undo step) |
| 22 | Take FX, item/take envelopes | partial — commands (`env.*` on takes, take chains in the model); no UI |
| 23 | Tempo ramps and meter changes on the timeline | done — in the ruler's tempo row: double-click edits or adds a tempo point, Alt+click toggles its ramp; in its meter half, double-click edits or adds a meter change (`timesig.set`) |
| 24 | Markers/regions: create, rename, navigate | done — M adds a marker at the playhead, Shift+R a region over the loop range; drag, double-click to rename, Alt+click removes; a ruler drag sets the loop range. [ and ] jump to the previous/next marker or region start |
| 25 | Piano roll, velocity and CC lanes | done — MIDI editor: piano roll, velocity lane, CC lane (mod, volume, pan, expression, sustain; draw/erase), notation panel |
| 26 | MIDI event list and notation views | done — notation panel; event list (every note and CC, text-edited cells, Delete, + Note) |
| 27 | Track automation lanes, read/write modes | done — volume/pan/mute and plugin-parameter lanes under their strip (click adds, drag moves, Alt+click deletes a point); header with mode, on, hide; touch/latch/write record fader moves while playing (`track.set`/`route.set`), latch/write hold to the stop. Plugin parameters are drawn and edited but not yet recorded from their controls |
| 28 | Automation items with pooled identity | gap — modelled (`ai_*`), no commands |
| 29 | Project/track/item timebase with visible consequences | gap (UI) |
| 30 | Track templates and FX chain presets | done — FX chain presets (`fx.chain.*`, FX window) and track templates (`track.template.*`, Track menu) |
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
| 39 | Isolated plug-ins with visible recovery state | partial — isolation in the engine; a per-FX badge (in-process / shared bridge / isolated, offline, failed) with Restart; isolated/VST3-bridged editors not embeddable yet |
| 40 | Reusable multi-action macros with typed inputs and undo | done — macros as one undo step, prompted inputs |

## Gaps found by this pass, and where they go

- Parent-send toggle (#7): fixed.
- Hardware input listing (#18): `engine.inputs` + the TCP input picker — done.
- Route channel maps (#4): engine — `route.add/set {channels}` over the existing
  `ChannelMap`; low value while strips are mono/stereo (#6).
- Stretch (#12), event list (#26), automation lanes and writing (#27), markers and
  regions (#24): done in this pass.
- Also closed: new MIDI items from the GUI (Item > New MIDI item, double-click on
  empty channel space), the item clipboard, the metronome, comping (#21),
  plugin-parameter automation recording (#27).
- Open: automation items (#28), screensets
  (#38), count-in, FX pin maps (#31), multichannel strips (#6).
