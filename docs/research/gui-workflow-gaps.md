# GUI workflow-gap log against `reaper-ux.md` §16 (P5.17)

Living document: each of REAPER's top-40 workflows (the research's
priority list) against what Quire's GUI does today. "Done" means a person can
do it in the GUI, through commands, with one undo step per gesture. Updated
2026-10-04 (orchestrator) on p5-gaps: every workflow is done.

Status: **done** · **partial** (what is missing) · **in progress** (owner,
branch) · **gap** (not started).

## P0 (1–20)

| # | Workflow | Status |
|---|---|---|
| 1 | Create a track instantly, rename in place | done — Ctrl+T / Track menu / TCP "+ strip"; inline rename in the TCP |
| 2 | Turn a track into a folder/bus, show parentage | done — `track.set {type}`, `track.move`; TCP indentation, type badge and parentage |
| 3 | Drag a track output to another track to send | done — drag a strip's "out" onto another strip (or its lanes); a click opens the routing matrix at the button |
| 4 | Inspect/edit each send (source, destination, channels, tap) | done — matrix details row: tap, level, pan, mute, polarity, channel map (swap, one side to both, mono, and the pairs both strips have: 1/2 → 3/4 …), remove; `route.add/set {channels, mono, phase_invert}` |
| 5 | Routing overview with legible cycles/sidechains | done — routing matrix: blocked cells say why (cycle path, strip rules), keys as destinations; a graph view is P1 |
| 6 | Track channel count, routes to 3/4 and beyond | done — ADR-0020: a strip is its main pair plus auxiliary channels 3-8 (`track.set {channels}`, Track > Channels…); routes map to and from them (linear, latency-compensated, real time = offline) |
| 7 | Parent/master send independent of explicit sends | done here — `track.set {parent_send}` and a toggle in the routing matrix (the outlined parent cell) |
| 8 | Drop audio/MIDI files onto tracks and timeline | done — File > Import and drag-and-drop onto the lanes (first file on the channel under the pointer, the rest on new tracks; one transaction) |
| 9 | Move, trim, split, duplicate with direct gestures | done — move (snapped, across channels), trim (left edge keeps the material), Ctrl-drag copy, S splits at the playhead, Ctrl+C/X/V and Ctrl+D (paste reproduces the items as copied, even after a cut), Ctrl+A; one undo step each |
| 10 | Draw fades, change crossfades | done — fade handles (Ctrl+click cycles the shape); overlaps made by a move, copy or trim crossfade automatically |
| 11 | Slip media inside an item | done — Alt-drag |
| 12 | Stretch with rate/pitch mode visible | done — Alt on an item's right edge sets length and play rate together; the label shows the rate and "varispeed" when pitch is not preserved |
| 13 | Switch takes, see the active take | done — the take badge cycles the active take |
| 14 | Searchable command list from anywhere | done — actions list (Shift+/), every command + built-ins + macros |
| 15 | Assign shortcuts, show conflicts by context | done — Ctrl+B in the actions list, keymap sections, user deltas |
| 16 | Add, reorder, bypass, wet/dry FX quickly | done — the **fx** button on every track (straight to the plugin browser on an empty chain) and **+ FX** in the mixer; FX chain window + plugin browser (favourites, stock first; the stock plugins built beside the app are found without setup) + parameter panel |
| 17 | Render master with clear source/bounds/format/destination | done — render dialog with live dry-run file names, jobs, add-to-project |
| 18 | Arm, pick input, monitor, record without hidden state | done — arm/monitor/input per strip (the picker names the device's ports from `engine.inputs`), record/loop/punch on the transport bar, a metronome (live output only), a count-in of 1, 2 or 4 bars (the transport's "count" button; also before playback if asked), and the record folder shown while a track is armed |
| 19 | Save/reopen with stable IDs and readable diffs | done — File new/open/save/save as, canonical JSON, confirm before discarding |
| 20 | Undo/redo every edit, expose command results | done — history dock (versions, branches, jump), command log (`log.list`) with origins |

## P1 (21–36)

| # | Workflow | Status |
|---|---|---|
| 21 | Take lanes, fixed-lane comping | done — an item with several takes shows them as lanes; click a lane to hear that take, swipe a range to comp it in (split + take, one undo step) |
| 22 | Take FX, item/take envelopes | done — take chains play in the engine (real time = offline); the item menu opens Take FX… and shows/adds take volume, pan and mute envelopes, drawn and edited on the item |
| 23 | Tempo ramps and meter changes on the timeline | done — in the ruler's tempo row: double-click edits or adds a tempo point, Alt+click toggles its ramp; in its meter half, double-click edits or adds a meter change (`timesig.set`) |
| 24 | Markers/regions: create, rename, navigate | done — M adds a marker at the playhead, Shift+R a region over the loop range; drag, double-click to rename, Alt+click removes; a ruler drag sets the loop range. [ and ] jump to the previous/next marker or region start |
| 25 | Piano roll, velocity and CC lanes | done — MIDI editor: piano roll, velocity lane, CC lane (mod, volume, pan, expression, sustain; draw/erase), notation panel |
| 26 | MIDI event list and notation views | done — notation panel; event list (every note and CC, text-edited cells, Delete, + Note) |
| 27 | Track automation lanes, read/write modes | done — volume/pan/mute and plugin-parameter lanes under their strip (click adds, drag moves, Alt+click deletes a point); header with mode, on, hide; touch/latch/write record fader moves while playing (`track.set`/`route.set`), latch/write hold to the stop. plugin parameters record the same way (`fx.param.set`) |
| 28 | Automation items with pooled identity | done — `autoitem.*`; in a lane: right-click inserts, duplicates (pooled or a copy), unpools, loops, deletes; the header moves an item, its right edge resizes it, a click inside edits its points (the pool follows) |
| 29 | Project/track/item timebase with visible consequences | done — positions stored in their timebase's domain; item, track and project timebase in menus (Project menu, Track > Timebase…, item menu), "time"/"pos" badges on items; tempo edits move what follows the beat and keep what follows time |
| 30 | Track templates and FX chain presets | done — FX chain presets (`fx.chain.*`, FX window) and track templates (`track.template.*`, Track menu) |
| 31 | FX pin maps, detector input on 3/4 | done — ADR-0020: `fx.set {pins: {in, out, key}}` and the FX chain's pins menu (on strips with auxiliary channels); key routes (ADR-0018) remain the short form |
| 32 | Render selected tracks/items/regions as stems | done — render dialog stems/regions |
| 33 | Queue exports, second output format | done — WAV or FLAC, a second file in another format/bit depth from the same render, and a render queue (`render.queue.*`; Add to queue / the queue panel in the render dialog) |
| 34 | True-peak/LUFS normalization, render measurements | done — normalize/target/limit in the dialog, loudness in results |
| 35 | Loop record into takes, punch modes | done — recording while looping makes one take per pass (ADR-0014/15); loop and punch toggles on the transport bar; a drag in the ruler's bars row sets the loop range, in its time row the punch range; the ruler's menu copies one range to the other |
| 36 | Peaks cached, never blocking editing | done — worker-built pyramids, disk cache |

## P2 (37–40)

| # | Workflow | Status |
|---|---|---|
| 37 | Contextual mouse gestures, toolbar/menu customization | done — a mouse map per context (item drag/edge/click, fade handles, empty lanes, automation lanes, ruler markers and tempo points) in mouse.json, edited in Edit > Mouse modifiers; the toolbar and menus are edited in the app (actions list Ctrl+T / Ctrl+M, a toolbar button's right-click menu) and saved to menus.json |
| 38 | Screensets and track layouts | done — named screensets (View > save/load/delete; slots Ctrl+Alt+1-5 save, Alt+1-5 recall) of the panels, zoom, scroll, follow and row heights (per track: drag a strip's bottom edge, double-click resets), in the project's UI file |
| 39 | Isolated plug-ins with visible recovery state | done — a per-FX badge (in-process / shared bridge / isolated, offline, failed) with Restart; editors of isolated (and bridged VST3) plug-ins embed from the bridge process into the app's window, and close if the bridge dies |
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
- Closed on p5-gaps (2026-10-04): count-in and the record path (#18), the
  punch range in the ruler (#35), route channel maps (#4), timebase (#29),
  screensets and track heights (#38), FLAC / a second file / the render queue
  (#33), take FX and take envelopes (#22), automation items (#28), the mouse
  map and in-app toolbar/menu editing (#37), bridged editors (#39), auxiliary
  channels and plug-in pins (#6, #31; ADR-0020).
- Known limits: auxiliary channels are not metered and do not reach hardware
  outputs or stem renders; pins are for track chains (take and input FX stay
  stereo); a pinned stage bypasses without a crossfade.
