# The DAW GUI

This spec describes the GUI as built and is updated with it. Decisions it
builds on:
ADR-0005 (SDL3 X11 windows, GL3 + NanoVG, own retained toolkit, performance
gates), ADR-0007 (command bus), ADR-0008 (versions, branches, undo),
ADR-0019 (typed strips), ADR-0030 (main-thread notifications/deadlines),
`ARCHITECTURE.md` §3 (threads, event delivery) and §10, and the workflow research in `docs/research/reaper-ux.md` (the top-40
list in §16 is the priority order for workflow coverage).

Goal: a complete track can be produced in Quire without falling back to
another DAW for core tasks. Performance gates (ADR-0005 §5): p95 frame
≤ 8 ms CPU at 1080p with a 64-track session in the arrange view at any zoom;
idle CPU ≈ 0; cold start to an interactive window < 300 ms; no UI work can
stall audio.

## 1. Process and threads

The GUI application is the engine process with a window: one binary
(`<slug>`, from `cmake/Product.cmake`; the headless engine stays
`<slug>-engine`). The UI thread **is** the model thread
(`ARCHITECTURE.md` §3): it runs the command bus, owns the session, services
CLAP main-thread callbacks and hosts plugin GUIs. Consequences:

- **Every change is a command.** Views dispatch typed commands with
  `Origin::Gui` through the same dispatcher as MCP, Lua and the agent panel.
  The GUI has no private mutation path; a gesture that needs many edits is a
  `batch` (one undo step). Continuous gestures (fader drags, item drags) use
  the bus's coalescing (one version per gesture, intermediate previews as
  coalesced edits — see §4.3).
- **The UI reads, never waits.** It renders from the current immutable
  session version (structural sharing makes holding a version cheap) and
  from telemetry rings (meters, playhead, scopes). Nothing in the UI calls
  into the audio thread or blocks on workers.
- **Long work leaves the thread.** Peak building, waveform decoding, plugin
  scanning, renders, analysis, file dialogs' I/O and Lua scripts run on
  workers and commit results as commands or post completion events. Budget:
  no synchronous task on the UI thread over 2 ms (`ARCHITECTURE.md` §3),
  measured (§9). As built: the browser's Rescan runs `plugin.scan
  {wait: false}` (every module in the supervised scanner, the catalogue
  installed when its job is done); the UI sidecar's coalesced writes go to a
  writer thread (exit and project switches write synchronously, after it);
  waveform lookups of media without a content hash read the file's size and
  mtime at most every two seconds and again on each new session version.
- **Event-driven loop.** `SDL_WaitEventTimeout` with the timeout set by the
  nearest deadline: app service (`engine.md` §6), local CLAP timers (also
  without editors), ModelLoop timers, pending notation compilation (300 ms),
  dirty render previews (40 ms by default), and armed UI sidecar checks.
  One parked `FdWatcher` watches the app relay, local plug-in descriptors and
  ModelLoop's wake pipe; readiness posts an SDL wake, dispatches on main, and
  stays out of poll until consumed/rearmed. Posted worker results also wake SDL.
  Signal shutdown wakes through ModelLoop's pipe even without any deadline.
  The 100 ms service and 10 ms control polls are removed: MIDI batches use the
  relay. Remote instances use only `setMainThreadWake` forwarding; the GUI
  does not poll a remote editor or its bridge descriptors. Playback frames
  run at 16 ms; transport counters refresh at 250 ms only while playing or
  recording, with stopped state/xrun/error changes refreshed on notifications.
  Visible moving/decaying meters have a 33 ms frame deadline, including their
  1.5 s peak hold; at rest the feed arms/rechecks one audio activity wake at
  its -60 dB display floor. Clip latches alone do not animate. Idle with no
  work or deadlines blocks indefinitely. FD service failure is reported and
  ends the app loop rather than adding a fallback poll. Offscreen idle
  measurements use the same service/watch/deadline path with a finite end bound.

### 1.1 Observing the model

Views observe the session through the bus's event stream (per-topic
sequence numbers; `event.dropped` → resnapshot, `ARCHITECTURE.md` §3). Each
view keeps a **view model** keyed by stable object ids (`tr_12`, `it_7`…)
and keys its snapshot cache by (project generation, version). Project
replacement forces a full resnapshot, even when ids and versions repeat.
Within a generation, a version change diffs the objects it shows (the model's
`diff(versionA, versionB)` already exists for ADR-0008) and invalidates only
the affected nodes. A view never caches pixels across versions without a
key that includes the object's content revision. The arrange view uses the
version store's change list for adjacent edits and undo within the same
generation; branch jumps compare
shared object identities. Track geometry changes rebuild the rows; other
edits update affected controls, item indexes and old/new item footprints.
Unrelated FX parameter edits preserve TCP control/capture identity and do
not rebuild or repaint the arrange view. Envelope glyphs stay within their
lane or take's item.

### 1.2 UI state is not session state

Zoom, scroll, selection, window layout, open editors, plug-in editor
placements, docking and
screensets live in a separate per-project UI file (`<name>.ui.json`,
canonical JSON, never in the project's diff) plus per-user settings
(keymaps, toolbars, theme) under the user config directory
(`$XDG_CONFIG_HOME/<slug>/`). Selection is exposed read-only to agents
(`gui.selection` query) because "the selected items" is useful context;
agents never depend on it for correctness.

Before project replacement, the old project's sidecar is flushed. The new
project's sections reset to their registered defaults before loading its
sidecar; absent files or sections retain those defaults. Sidecars use
`supple.ui/1.0`; a file without a `schema` reads as 1.x, and a different
schema major is treated as absent and its file is left untouched. Save As preserves the current layout and copies it to
the new sidecar, subject to the same schema protection.

Any input event (including editor geometry) or command processed on main arms
one compare/write check about a second later. Continued input coalesces into
that earliest check, so long gestures still persist; unchanged idle UI has no
periodic check. Internal wake events do not arm writes. Writer completion/failure
posts to main; failures arm a bounded one-second retry. Project switches and
exit retain explicit flushes.

Screenset names trim leading and trailing spaces and tabs. Import skips
empty names and silently keeps the first entry when trimmed names collide.

## 2. Toolkit (`daw/gui/ui`)

A small retained-mode toolkit, ours (ADR-0005 §4). Not a general framework:
only what the DAW needs, designed for virtualized, data-heavy views.

- **Nodes.** A tree of `Node`s (id, role, bounds, children, flags). Roles
  (`button`, `slider`, `list`, `row`, `track`, `item`, `text-field`…) are
  semantic: they drive keyboard behaviour, UI tests and, later, AT-SPI
  accessibility. Custom-drawn views (arrange, piano roll) are one
  node each with their own hit testing and internal virtualization.
- **Layout.** Row/column stacks with fixed / fraction / content sizing, gaps,
  padding, min/max; absolute placement inside timeline-like views. Layout is
  pure (inputs → rectangles), cached per node, recomputed only when size or
  content revision changes. All layout in *design units*; the window's
  display scale (SDL) maps them to pixels, so fractional scaling is a single
  factor.
- **Input.** Hit testing front-to-back, pointer capture during drags,
  hover, wheel with precision (trackpad deltas), modifiers, double-click.
  Keyboard: focus chain, Tab navigation, shortcuts resolved by the actions
  system (§6) with context sections (arrange, MIDI editor, mixer, text
  field). Text input through SDL3 text-input events (IME composition shown
  inline). Hiding a dock or panel releases focus and pointer capture within
  its subtree (`Root::releaseWithin`), sending `Cancel` to the captured
  control. Continuous edits end as one undo step; uncommitted drag previews
  and pending button clicks are discarded. Focus and capture outside the
  subtree are preserved. A closing popup restores focus only to a node
  still in the tree, visible through all ancestors and enabled through all
  ancestors; otherwise it restores no focus.
- **Damage.** Each window keeps a dirty-rect list; nodes invalidate
  themselves; a frame redraws only damaged regions (scissored). Removed nodes
  survive the current input dispatch; focus, hover and capture are resolved
  again after callbacks. Views own overlay registrations and release them
  when destroyed. Animated
  nodes (meters, playhead) are separate *layers* with their own small dirty
  rects so a running transport does not repaint the arrange view.
- **Rendering.** Per window one GL 3.3 core context.
  - **Chrome** (widgets, text, icons, outlines) through NanoVG GL3.
  - **Bulk layers** in plain GL with persistent buffers: waveforms (§3.1),
    MIDI notes as instanced rectangles, envelopes as line strips, meters
    and spectra as small textures/buffers. Bulk data never goes through
    NanoVG (the spike measured 81 ms/frame doing so).
  - Order per frame: background → bulk layers (scissored per lane) →
    chrome → overlays (drag ghosts, menus, tooltips).
- **Text.** NanoVG fonts: Inter (UI) and IBM Plex Mono (numbers, notation,
  code), OFL-1.1, bundled (`assets/fonts/`, recorded in `THIRD_PARTY.md`).
  Tabular figures for every numeric readout.
- **Theme.** A token set (colours, type scale, spacing, radii, stroke widths,
  motion durations, strip-type colours) loaded from JSON; light and dark
  variants; widgets read tokens only, never literals. Extends the plugin
  template's `Theme.h` contract so stock plugin editors and the DAW share
  tokens. Colour values are `#` followed by exactly six hexadecimal digits.
  Sizes (`type`, `space`, `radius`) are finite, non-negative numbers within
  the nonzero range of `float`, or zero. Invalid values produce structured
  errors with escaped JSON-pointer paths. Theme and arrange hex colours share
  the same strict colour parser.
- **Animation.** Explicit, short, interruptible (eased, ≤ 150 ms) and only
  on state changes the user caused; animation requests frames, idle stops
  them.
- **Testability.** The toolkit renders into an offscreen framebuffer
  (SDL's offscreen video driver or EGL surfaceless) for golden-image tests
  (per-pixel tolerance, stored as PNG under `daw/gui/tests/golden/`), and a
  test API drives nodes by id/role (click, type, drag) and reads back state.
  GL readback, not XGetImage (ADR-0005 open item).

## 3. Data-dense rendering

### 3.1 Waveforms

A per-media **peak pyramid**: min/max (and RMS) per channel at block sizes
64, 256, 1024, 4096, 16384 samples, built by an I/O worker on import or
first display, cached on disk under the user cache directory keyed by media
content hash (never in the project, `reaper-ux.md` §5). The arrange view
picks the coarsest level with ≥ 1 bucket per pixel column, uploads one
vertex pair per column per visible item into a persistent buffer, and draws
the visible span; scroll shifts the buffer window, zoom rebuilds only
visible items. While a pyramid builds, the item draws a placeholder and a
progress hint; editing never waits for peaks. Take rate/stretch maps
item-time to source-time before sampling the pyramid.

### 3.2 MIDI

Notes as instanced rectangles (position, length, pitch, velocity colour,
selection) from the item's sequence (the same compiled representation the
engine plays), in the arrange view (mini view) and the piano roll; CC lanes
as line strips/bars. Loop thumbnails include source-crossing notes held from
the preceding cycle, clip them at the item's boundaries, and omit source
note starts at or beyond the loop length, matching playback. Offsets and
play rates apply to these tails too; loop-pass and note-glyph drawing remain
bounded to 8192 each. Without a positive source length, the thumbnail uses
one nonlooping pass, as playback does.

### 3.3 Meters and telemetry

Meters read the engine's lossy telemetry rings (peak, RMS, and for masters
momentary/short-term LUFS from core-analysis) at 30 Hz, with peak hold and
clip latches as view state. The playhead reads the transport telemetry. No
meter ever forces a full repaint.

## 4. Views

Each view lists the commands it needs. A missing command is added to the
engine (with tests and generated docs) before the GUI uses it; the GUI
never works around the bus.

### 4.1 Main window

Transport bar (play/stop/record/loop/punch, position in bars|beats and
time, tempo/meter, CPU/xrun/anticipation readouts from engine telemetry);
toolbar (actions, §6: only what the transport bar does not already have —
undo, redo, add channel, the actions list by default); the arrange area (track control panel + timeline);
a bottom dock (mixer, MIDI editor, FX chain, routing, render queue, agent,
command log) with tabs; floating windows for plugin GUIs and any undocked
view. Window layout is UI state (§1.2).

### 4.2 Track control panel (TCP)

Per strip: name (rename in place: a double-click; Enter or a click anywhere
else keeps the edit, Escape drops it), **strip-type badge** (Channel / Buss /
Master: colour, shape and label differ — clear at a glance, ADR-0019),
mute/solo (rounded pills in their own colours), arm (a red disc: faded off,
solid armed), monitor (a speaker: an outline off, solid on, green auto),
input (channels only for these three), polarity ("Ø") and mono pills at the
start of the level row (ADR-0022), volume fader and pan
(Console 9 Channel's controls on channels and busses; plain gain and
balance on masters; the fader's dB scale comes from `libs/console9`'s
mapping), "fx" (the chain window) with "+" beside it (the plugin browser
alone, adding last in the chain; no count, to save room), the "out" button
(a click opens the routing matrix; drag onto another strip to create a
route; drop rules follow `route.strip_type`, invalid targets show why before
the drop; a strip the track already sends to is not a target, since the
matrix shows one send per pair), meters. The mixer's strips carry the same controls in the same
looks (§4.4). The button row starts at the TCP's left edge with mute and
solo (over polarity and mono) and ends at its right edge with "env" and
"out", at any depth; a channel's input button takes the room between.
Every view names a track the same way (`model::trackLabel`):
its name, which track.add sets to the first free `ch_N`, `buss_N` or
`mstr_N` (the session's own master is "Master"); an unnamed track from an
older session shows that kind of label too.

A strip control used on a selected track acts on the whole selection, in the
TCP and the mixer alike (`gui::SelectionEdit`): mute, solo, polarity, mono,
arm, monitoring and the input set the same value on each (arm, monitoring and
input only on its channels); a fader or pan drag moves each selected track by
the same amount from where it started (clamped). A click is one undo step, a drag is
one. A control on a track outside the selection acts on that track alone.
Folder/buss parentage shows as indentation and a parent bar. Under each
parent the channels are listed first, then the busses (each in the parent's own
child order, `views::displayChildren`), so a buss never sits between two
channels; the model's child order is not changed (the mixer groups by type
too, §4.4). Dragging an empty part of a row (not a
button) reorders the track among its siblings *of the same type* under the same
parent: an insertion marker shows the drop, Escape cancels, and the release is
one `track.move {track, index}` (`views::reorderIndex` computes the index;
no-op drops commit nothing). A buss cannot be dropped between channels, and
a track does not change parent by dragging. The panel's foot ("mixer",
"+ strip") is a band the rows never cover; the tracks scroll far enough for
the last one to clear it. Adding a strip
offers exactly **Channel, Buss or Master** (`track.add {type}`); a
double-click on the panel's empty space (or the mixer's, §4.4) adds a channel
and selects it; a right-click there opens the same Channel / Buss / Master menu
at the pointer. Track selection is shared by the TCP and the mixer: a click on
a row's or strip's empty part selects it alone, Ctrl+click toggles, Shift+click
extends from the last click in that view's order; a right-click on a selected
track keeps the selection. The track menu's Delete (and the Delete key, when
tracks were selected after items) removes every selected track as one undo
step (`track.remove:selected`; the main master stays; children before folders).
Commands: `track.*`, `route.*`, `fx.*` (bypass/offline), transport.

### 4.3 Arrange view

Ruler (bars/beats and time; tempo and meter lanes; markers and regions),
virtualized track rows (only visible rows exist), items with waveforms or
MIDI, take lanes, fades and crossfades, automation lanes (volume/pan/mute
on strips; plugin parameters), time selection, snapping grid, playhead with
follow modes. On every version change, deleted track and item IDs are
pruned from transient selection; undo restores objects, not old selection.
MIDI editor selection clears on external version changes, and a gesture
cannot commit against a different project generation or version from the
one where it started. An automation point drag (on an envelope or an
automation item) is dropped, with an `env.changed` message, when the points
changed elsewhere between the press and the release.

Gestures (P0, `reaper-ux.md` 8–13): drop files onto tracks/timeline, move,
trim edges, split (S), duplicate, slip (Alt-drag), stretch (with the rate
and pitch-preservation mode visible), fade/crossfade handles, take switch.
Each gesture previews locally (a ghost) and commits **one** command or batch
on release, so a drag is one undo step and never floods versions;
continuous controls (faders) send coalesced `*.set` edits during the drag
and one final value (the bus's gesture coalescing). A pooled TCP row pins
the original track for fader, pan, resize and rename gestures; rebinding
cancels the gesture. Wheel zoom holds the **project cursor** (the playhead,
the transport position the position display shows) still by default, or the
mouse pointer when the mouse setting `wheel.zoom` is `mouse_pointer` (Edit >
Mouse modifiers, or View > Wheel zoom anchor); a cursor outside the visible
lanes falls back to the pointer. Keyboard zoom anchors at the playhead. The
timeline's start stops the view, so zooming out near it pulls the anchor left
on screen; zooming back in returns it to where it was, as each step holds
the anchor where the first step of the run found it. The run ends when anything
else moves the view in time (a scroll, follow, the scrollbar), the cursor
moves, or (with the pointer anchor) the pointer moves. Horizontal zoom is continuous;
vertical track height per track and global.
Left trim shifts every take's source offset by the trimmed duration times
its play rate, in the same transaction; dragging the left edge out past a
take's material (down to the timeline's start) and slipping its material
later both leave silence first (a negative offset), audio and MIDI alike. Clipboard track offsets are in
channel order; pasting skips envelope rows and reports items dropped for
lack of a target channel. Project replacement clears the item clipboard.
MIDI thumbnails map offset, rate and loop passes
as playback does. Fade shapes and curve bends share playback's evaluator.
Automation drawing samples only visible time and bounds loop glyph work;
plugin automation interpolates in the playback parameter domain (ADR-0010).

**Takes.** An item with several takes shows each in a lane under its name
bar when the row is tall enough (at least 14 units a lane), the heard one
lit and each lane named; a click on a lane hears that take, a swipe comps it
in, and the badge cycles takes. The item menu (right-click) adds, for
several takes: **Show all takes** (a check; UI state, saved in the project's
UI file as `expanded_takes`): the item's row grows to the name bar plus one
lane per take, each as tall as the row's own height, so every take can be
seen, heard (click) and edited; **Explode takes onto new tracks** and **…in
sequence on this track** (item.explode-takes, recording order; this ends
the takes, Show all takes keeps them); and **Delete take "name"** for the
take under the pointer (else the heard one; take.remove). For any item with
material: **Convert to audio** / **Render to audio as a new take** on a
MIDI take, **Render with FX, replacing the audio** / **Render with FX as a
new take** on an audio take (item.apply-fx through the take and track FX;
the take under the pointer is heard first). **Ctrl-drag** below the name
bar of an item with several takes takes that take (the lane under the
pointer, else the heard one) out into an item of its own, anywhere on any
channel (take.extract, one undo step; elsewhere Ctrl-drag still copies).

**The marquee.** A drag from empty space, or a right-drag anywhere in the
lanes (on an automation lane: a right-drag, or a Ctrl-drag from empty
space), draws one marquee that selects every item it touches and every
automation point inside it, on any track and in any lane, take envelopes
drawn on items included; with the add modifier (`lane.drag`) it adds to
what is selected. After a marquee that took items and points, Delete
removes both (one undo step) and Ctrl+C, Ctrl+X and Ctrl+V take the items;
after one that took points only, the keys take the points.

**Automation lanes.** A click on a point selects it (Ctrl+click adds or
removes one) and a click on empty space adds a point there, selected.
Dragging a selected point moves every selected point, in every lane, by the
same time and the same share of its lane's height (one `env.points.set` per
envelope, one undo step); Shift frees it from the grid and Alt+click
deletes a point. A click selects in one lane; the selection names points
by position and value, so points an edit elsewhere removed simply leave it;
a press elsewhere in the lanes clears it. While a lane was clicked last,
Delete, Ctrl+C, Ctrl+X and Ctrl+V take the selected points instead of
items; points copied from one lane paste at the playhead into the lane
clicked last, points from several lanes back into their own lanes, keeping
their offsets in time. A right-click
without a drag opens the lane's menu: **Insert point**; **Select all
points**, **Copy**/**Cut**/**Paste points** (paste at the pointer, replacing
the points over the pasted span), **Delete points**, **Point shape…** (a
second menu: linear, square, slow start/end, fast start, fast end, bezier);
**Insert automation item**, or for the item under the pointer **Duplicate**
(pooled, or a copy), **Unpool**, **Loop** and **Delete**; and **Bypass**,
**Hide** or **Remove envelope** (`env.set active/visible`, `env.remove`).
Points pasted into an envelope of another kind (volume into pan) keep their
height on the lane. An automation item's header moves it; dragging its right
edge stretches its shape with it (its points and the span they repeat over,
for every item of its pool), and Alt-drag extends or shortens it by
repeating the shape instead.

Commands: `item.*`, `take.*`, `fade.*` (as named in session-model §7),
`envelope.*`, `marker.*`, `region.*`, `tempo.*`, `transport.*`.

### 4.4 Mixer

Strips in track order grouped by type: channels, busses, masters (several
masters are allowed, ADR-0019), each with the type's visual language; FX
slots, sends (level/pan/tap, with "follow" pan shown), input/arm/monitor on
channels, fader + pan + meters, Console 9 on/off as a project toggle with its
level-matched behaviour explained in its tooltip. Meters update only when
their entire ancestor chain is visible, and clear peak hold on reopening.

Small "fx" and "+" buttons sit at each strip's top right (the TCP's: the
chain window, and the plugin browser alone); a double-click on the name
renames the strip in place, as in the TCP. Under the chain (and its sends),
the arm, centred (channels); an input button (channels) and "out" (not on
masters: a click opens the routing matrix, a drag onto a buss or master makes
a send); then one row with the small polarity pill ("Ø"), monitoring
(channels) and the mono pill (ADR-0022), above the pan. Everything under the
chain has a fixed height and the chain takes what a strip's missing rows
leave, so every strip's fader sits at the same height. The strip's plug-in chain lists every plug-in,
scrolling when it is longer than its section, and takes the room a strip's
unused rows would leave (sends show only when there are some, up to two;
busses and masters have no arm row or monitoring). Alt+click on a plug-in removes it
(`fx.remove`, one undo step); a double-click on the chain's empty part opens
the plugin browser for that strip. A double-click on the strips' empty space
(not on a strip) adds a channel; a right-click there offers Channel, Buss or
Master, as the TCP's "+ strip". A send dragged from "out" draws a wire from the
button and outlines the strip it would feed, as in the TCP. The
panel's top edge (shared with the MIDI editor: it lights up under the pointer)
resizes the mixer by drag and only the chain
sections follow (meter, fader, buttons keep their size); a double-click on it
restores the default, which fits five plug-ins. The height is clamped like the
MIDI editor's (what the center column leaves less a strip of the shell) and
saved in the UI sidecar's `mixer` section. Dragging a plug-in within its chain
reorders it (`fx.move`); dropping it on any part of another strip copies it, with
its live settings, last in that strip's chain (`fx.copy`). Dragging a strip's
empty part moves it among the strips of its own type only (`track.move`, an
index among its parent's children; `views::reorderIndex` in `track_layout.h` computes it, shared
with the TCP). A drag shows an insertion bar or a target outline; Escape or a
drop outside the strips cancels; each is one undo step.

### 4.5 FX chain and plugins

Native editor parents are tagged X11 XIDs, Cocoa NSViews or Win32 HWNDs
(ADR-0040). Local CLAP and VST3 views embed into an SDL editor container.
Windows bridged views use the same HWND across processes. macOS bridged views
float in a bridge-owned NSWindow; no empty SDL editor window is created.
The bridge applies resize/show/hide and user-close itself and reports closure
through its existing notifications. Replacing a Cocoa bridged instance opens a
fresh child-owned window; the other editor paths retain their SDL container.
Cocoa bridge placement and focus are not
persisted or included in the DAW's MIDI-learn focus predicate. Floating-level
windows can remain above unrelated applications; cross-process transient
ownership is unavailable. Windows keyboard accelerators/IME depend on the
plugin's native child; the DAW does not forward shortcut messages.

Window sizes/placements remain design units. Pixel editor sizes on
Linux/Windows use pixelRatio when converting observed geometry/placements;
container sizing goes directly from editor pixels to SDL window coordinates
using pixel density, avoiding integer design-unit rounding at fractional DPI.
Cocoa editor sizes are already points.
The GUI worker's inputScale maps SDL event coordinates to design units. Opening
uses the current scale; live scale-change renegotiation requires close/reopen.
Native run loops deliver local editor input; timers remain GUI deadlines.
Linux/macOS FD readiness uses the shared watcher; Windows advertises no CLAP
POSIX FD support. Display tests remain opt-in, never enabled by normal CI.


Chain list with add (searchable browser over the plugin scan cache;
favourites and stock plugins first; the browser also opens on its own from a
strip's "+", titled with the track and with its own Close), reorder by
dragging a row (an insertion bar; Escape cancels; Alt+Up/Down on a focused
row too; no arrow buttons), bypass vs offline
(distinct, `reaper-ux.md` addendum), a double click on a row opens that
effect's own GUI, Ctrl+click adds a row to the selection (or takes it out)
and Shift+click selects the rows from the last one clicked; on a row of a
multi-selection, bypass, offline and Remove act on every selected effect in
one undo step (the editor button and a double click open only that row's
editor), wet/dry, per-FX latency and CPU, preset
menu, and the plugin's own GUI (native view/child of an SDL window, or a
bridge-owned Cocoa window; ADR-0040, `clap.timer-support`, resize negotiated from observed
geometry). For plugins without a GUI — and always as a second tab — a
**semantic parameter panel** generated from the plugin's semantic schema
(units, ranges, enums, groups): the same description an agent reads, so the
human and the agent see the same names and units. Stock plugin editors are
the template's Pugl editors. Every writable parameter row of the panel
(semantic, and a plain plug-in's listed parameters) has a **MIDI** button
that names its learned control ("CC 21") or opens MIDI learn (ADR-0027): the
learn dialog says "Move the MIDI controller you wish to assign", shows the
control moved last (kind, number, channel, input), offers the CC mode
(absolute, relative 1/2/3, toggle), any input or only that one, enable only
when the effect's track (or a take FX's item) is selected, when its FX window
or editor has the focus, or when one of them shows, and soft takeover; OK
binds through `fx.learn.set`, Remove (when bound) through `fx.learn.remove`.
While the dialog is open nothing learned moves. Latency badges refresh after effect model edits,
host replacement and live latency changes. File-descriptor service startup
and poll failures are reported; editors requiring unavailable FD service
cannot remain open. App instances use the app-wide local descriptor snapshot; caller-owned editor
instances use their editor serial. The registrations share one watcher, including
when no editor is open. Watches suspend before model/input commands can close
or reuse descriptors, and a poll overlapping a changed set is discarded.
App timers are serviced once by EngineApp; editor service adds deadlines only
for caller-owned local instances. Readiness reaches its original registration. Plugin show/hide requests change the parent window's
visibility while retaining the editor and its timer/FD service. Close takes
precedence; when show and hide are coalesced together, hide takes precedence.
Opening an existing hidden editor shows and raises that same window. Each
effect's editor reopens where it was last, at its last size when it is
resizable (a plug-in may adjust the size it is offered); placements are kept
per effect in the UI file's `plugin_windows` section (`{fx id: {x, y, width,
height}}`), so they survive closing the project. A position no longer on any
display (a monitor that is gone) is not used. The header's **Save chain…**
shows a name field for a chain preset (fx.chain.save) and **Load chain…** a
menu of saved presets (fx.chain.list; a click appends, Shift replaces);
pressing either button again while its field or menu shows closes it. A right
click on a preset in the menu offers Delete (fx.chain.delete). Commands:
`fx.*`, `plugin.*` (scan/describe), `param.*`, `patch.*`.

### 4.6 Routing view and matrix

One grid (`reaper-ux.md` addendum 2): sources (strips in hierarchy, hardware
inputs, MIDI inputs) × destinations (parent, strips, plugin keys, hardware
outputs); a cell creates/edits a route (tap, channels, level, pan/follow,
mute, invert, mono); cells that `route.strip_type` would reject are drawn
disabled with the rule as their tooltip; record-input assignments appear in
the same grid. A graph view (nodes and edges, PDC per node) is P1. Strips are listed in the
views' order (`route.matrix`): each master's tree with a parent's channels
before its busses, then the masters at the bottom, under the views' labels
(§4.2). A strip's "out" button opens it (TCP and mixer). A strip with
auxiliary channels (a multi-out instrument's outputs, ADR-0021) has a row per
channel pair under its own, named by the outputs ("3/4 Kick"); a cell there
sends that pair to the destination's main pair, and its channel menu keeps
the pair while choosing the destination's.
Matrix updates clamp scroll to the current endpoint range; paint uses
endpoint-pair indexes for send and cycle rules.
Commands: `route.*`, `track.set {record_input}`.

### 4.7 MIDI editor

Piano roll (instanced notes; draw, select, move, resize, velocity drag,
quantize, transpose), velocity and CC lanes, and a **notation panel**: the
item's notation text (`libs/notation`) beside the roll, editable with live
compile diagnostics (the compiler's located errors and hints); edits in
either side update the other through `midi.edit` (event edits) and
`midi.write` (notation replacement). A draft that would replace MIDI changed
elsewhere since it was loaded is not written by the pause, leaving the panel or
closing the editor: the panel says so, and Ctrl+Enter applies it over the
change. This is the
LLM-native differentiator: the same text an agent writes is visible and
editable by the human. The event list edits notes and CC events;
double-click editing requires a row hit and edits that row. Roll gesture
anchors use exact model timing rather than the rounded `midi.notes` query.

The panel's top border is a drag handle: dragging resizes the panel (up is
taller; up to nearly the whole window, always leaving the transport and a strip
of arrange, and clamped again whenever the window shrinks or a neighbouring
dock grows), double-clicking resets it to the default height. The height is
UI state (the `midi` section of the sidecar, §1.2). Wheel in the roll: scrolls
pitch; Shift scrolls time; Ctrl zooms time around the pointer; **Alt zooms the
key/row height** around the pointer's pitch (4–48 px per key), and also works
over the keys, lanes and header. Row height is not persisted.

Native file-dialog completions check owner lifetime before posting and again
on the UI thread; outstanding dialogs may safely outlive shutdown.

### 4.8 Render dialog

A front end to `render.*` (`engine.md` §3): sources (master, stems, selected
items/regions, region matrix), bounds, tail, output pattern (the typed
variables of the render schema) with a **live dry-run preview** of the
resolved files, formats (primary + secondary), normalization/limiting,
add-to-project, queue. Every option maps to a schema field; nothing is
GUI-only. A nonempty invalid numeric field blocks preview, queue insertion
and rendering with a field diagnostic. Cancelled jobs remain labelled
cancelled and retain any partial result files returned by the engine.
The Add-to-project checkbox reflects its stored flag whenever the dialog
opens. A Folder row shows where the files go (`~` for the home folder) and
**Choose folder…** opens the system's folder dialog; the choice applies at
once and is kept as the `render_directory` preference (default: the media
folder, §4.11). The dialog tracks one render or queue job at a time; a second launch
is refused with a status message while it tracks that job.

### 4.9 Recording

Arm, input and monitor in TCP and mixer; record mode (normal, punch,
loop takes, ADR-0014/0015) in the transport.

### 4.10 Command log and history

A dock showing the command log (every command with origin — GUI, agent,
Lua, MCP — arguments, result/error) and the version history per branch
(undo/redo, jump to version, branch list). Agent commands are visibly
attributed.
The history panel refreshes on changes to the project generation, head
version, branch list or current branch name, including branch changes that
leave the head version unchanged.

### 4.11 Project files, folders and templates

File > New, Open…, Save, Save as… and Import audio… use the system's file
dialogs (asynchronous; results run on the UI thread), and ask Save / Don't
save / Cancel before unsaved changes are lost. A save (or project template)
that comes back with warnings (a plug-in whose state could not be read keeps
its earlier state) opens a popup listing them. Open and Save as start in the
project's own file, else in the media folder `~/Documents/<slug>-media`
(created on demand); any other folder can be browsed to. Renders go to the
same media folder unless the render dialog chose another (§4.8); under
`--workdir` (tests) they go to `<workdir>/renders`.

**Each project has a folder of its own**: Save as of `<folder>/<name>` saves
`<folder>/<name>/<name>.json`, with the project's `media/` beside it, unless
the chosen folder already is `<name>`; a new name chosen from inside the
current project's folder goes beside that folder, not inside it. Saving as
onto another existing project is refused with a message. With **Copy
imported audio into project** on (Preferences, the default), imports keep
their own copy in the project's media folder (session-model.md §5.3).

**Project templates** are whole projects kept by name in
`<config>/<slug>/templates/<name>/` with a copy of their audio
(project.template.*): File > Save template… asks for a name (a name in use
asks once more, then replaces it); File > Load template… lists them, a click
starts a new, unsaved project from one (after the unsaved-changes question)
with its own copy of the template's audio, and a right click offers
Delete.

## 5. Agent panel (`daw/agent-panel`)

- **Endpoints:** any OpenAI-compatible chat endpoint (including a local
  llama-server) or the Anthropic API; base URL, model and key (key stored
  in the user's keyring or a 0600 file, never in the project). Requests run
  on a worker thread; the UI streams tokens. A saved endpoint change applies
  from the next turn (a running turn finishes on its endpoint); the
  conversation and a pending proposal stay.
- **Tools:** the agent gets the same MCP tool surface as external agents
  (generated from the registry) plus the bundled skill as context
  (progressive disclosure: the entry file, then topics on demand). Its
  commands carry `Origin::Agent`.
- **Branches by default:** an agent session works on a new branch from the
  current version (ADR-0008). The panel shows the branch's **diff** against
  its base as a readable change list (objects added/changed/removed, with
  parameter changes in units) and offers **A/B playback**: the transport
  switches between the base and the branch at the same position (the
  engine renders whichever version is selected; switching is a plan swap,
  crossfaded). **Accept** merges (fast-forward or the replay merge), **reject**
  deletes the branch. Every agent action is in the undo history and the
  command log.
- **Proposal identity:** a proposal belongs to the project generation and
  path at branch creation. Audition, diff, accept, reject and follow-up send
  check both and refuse a mismatch with a message that the proposal belongs
  to a different project. Save As also changes this identity through the path.
- **Listening requests:** when the skill says "ask the human to listen",
  the agent posts a listening card (what to listen for, which region, A/B
  targets) instead of deciding alone.

## 6. Actions, keys, toolbars, menus

- **Actions** = a command plus bound arguments (and optional argument
  prompts), named and searchable (`reaper-ux.md` addendum: ~4,400 REAPER
  actions collapse to ~2,000 shapes; parameterized commands cover them with
  far fewer entries). The registry's commands generate the base set;
  context-dependent arguments resolve from the selection ("selected
  tracks") at invocation.
- **Actions list** (`?`): fuzzy search over names, descriptions and
  commands; shows bound keys; runs, binds (Ctrl+B, then the chord), adds to
  toolbar/menu (Ctrl+T, Ctrl+M), and learns a MIDI control for it (Ctrl+L:
  the learn dialog of §4.5 with the CC mode and input choice). The list shows
  a bound control after the keys. A press (note on, CC above 0, program
  change, an encoder step) runs the action once; the "(MIDI CC)" actions
  follow the value instead — the selected tracks' volume (the fader's
  travel) and pan (an encoder step is 1 %), the master's volume, a beat
  jog of the play cursor, and the **last touched parameter** (the plug-in
  parameter edited last in the FX window or a plug-in's own editor; a knob
  takes over softly, an encoder at once; the FX window's tab row names it)
  — one undo step per movement. A control drives one
  action (learning it for another moves it); the bindings are user settings
  in `controls.json` (`supple.controls/1.0`: `{bindings: [{action, midi,
  mode}]}`, beside the keymap). Its keys work with the focus on the search
  field or on the list (after a click on a row), where typing goes on
  searching. A new binding goes to `global`; an `arrange` binding on the same
  keys gives way (the arrange view looks there first), and the hint names
  the action that had them. **Reset keys…** (also the action
  `ui.keys.reset`) asks first, then restores every default binding and saves
  a keymap file without changes. Selected-item actions cover the take
  features: explode onto tracks or in sequence, render with FX as a new take
  or in place, delete the heard take, show all takes (`ui.item.takes`).
- **Keymaps:** per context section (global, arrange, MIDI editor, mixer,
  text field), chords allowed, conflicts shown in the editor. A key looks in
  `text` while a text field has focus, in `midi` or `mixer` while the focus
  is inside that panel, then in `arrange` (its defaults edit the shared
  selection) and `global`, innermost first. Stored as
  canonical JSON in user config; defaults close to REAPER's common keys
  where they are not proprietary assets (key choices are not copyrightable
  expression; no REAPER files are copied). Among the defaults: Space
  play/stop, R record (also Ctrl+R), L loop, Ctrl+M or Alt+D the mixer,
  Alt+E the MIDI editor on the item it had last (else the selected or first
  MIDI item; nothing without one). Space is never a button's: buttons and
  toggles take Enter, so Space after a click still plays or stops, and a
  rename that ends gives the keys back to the shortcuts.
- **Macros:** a sequence of actions (or a Lua script) run as one batch (one
  undo step), with typed prompts; the values given reach every step, each
  taking the ones its own prompts name. Macros nest up to 16 deep.
- **Prompts:** an action run from a menu, the toolbar or a key that still
  needs typed arguments the selection does not supply opens the same form
  as the actions list.
- **Toolbars and menus:** built from actions; customizable; stored in user
  config (`menus.json`, supple.menus/1.0). Edited in the app: Ctrl+T in the
  actions list puts the selected action on the toolbar (or takes it off),
  Ctrl+M adds it to a menu or removes it; a toolbar button's right-click menu
  moves or removes it, or resets menus and toolbar.
- **Mouse modifiers:** a map per context of the arrange view — item drag
  (move, copy, slip; each also off the grid), item edge (trim, stretch), item
  click (select, toggle, extend), automation item edge (stretch, loop), fade
  handle (drag, cycle shape), empty-lane drag (select, add), automation lane
  (edit, toggle, delete), ruler markers/regions
  (move, remove) and tempo points (toggle ramp) — from each modifier
  combination to a behaviour. The defaults are the built-in gestures;
  `mouse.json` (supple.mouse/1.0, only what differs) overrides them, and
  Edit > Mouse modifiers edits the live map. One context, `wheel.zoom`, is a
  single setting rather than a modifier map: the wheel's zoom anchor
  (`project_cursor`, the default, or `mouse_pointer`).
- **Preferences** (Edit > Preferences…): app-wide settings, never project
  state. The default monitoring of new channels (off, on — the default — or
  auto; `preferences.json`, supple.preferences/1.4, sets
  `EngineApp::defaultMonitor`, so the agents' track.add follows it too), the
  wheel zoom anchor (kept in `mouse.json`), and a way into the mouse
  modifiers. **Audio processing threads** is an advanced live/background
  override: Automatic (default) or a limit per pool including its submitting
  thread; Limit 1 is serial. The menu
  offers 1, 2, 4, 8, 16, 32 and 64; `audio_threads` accepts any integer 1..64,
  with 0 meaning Automatic. Automatic uses the capacity derived from the CPUs
  the process may run on and
  graph-aware participation (engine.md §2.2, ADR-0023/0024). A note says it applies
  on the next engine start; changing it never restarts playback immediately.
  The limit applies independently to each pool, not total process threads;
  offline export keeps its separate thread setting. Version 1.0 files remain readable; missing `audio_threads`
  means 0, and saves write canonical 1.4. Invalid types/ranges return
  `preferences.audio_threads` at `/audio_threads` with a corrective hint;
  a failed save leaves the effective and displayed thread setting unchanged.
  **Lock process memory (mlockall)** is off by default, saved as the boolean
  `lock_memory`. Both 1.0 and 1.1 files remain readable; an absent field means
  false. Other types return `preferences.lock_memory`, `/lock_memory`, and
  "use true or false". The option applies on successful engine start or
  immediately when toggled while running. A failed save preserves the previous
  choice, following the existing audio-thread setter. A failed OS application
  keeps the saved/requested choice On with a failed effective note and errno
  plus a corrective hint. The row uses the same small grey left-column label
  and wide right-column On/Off button as the other preferences. Its two short
  caution lines explain RAM pressure, sample-library/stack failure, host-only
  scope and that turning it off also drops plug-in locks (engine.md, Optional
  host-process controls); a third says how to grant the permission it needs
  (an unlimited memlock limit for the audio group in
  `/etc/security/limits.d/`, and membership of that group). The status note appears only for pending intent
  ("Applies when the engine starts"), effective locking ("Locked") or failure;
  Off has no duplicate note or reserved space. Notes refresh on opening/toggling
  and model/service notifications; the dialog height follows visible
  content. Canonical saves
  preserve existing monitoring/thread values; no project format changes.
  **Keep CPUs out of deep idle** is separately off by
  default, persisted as boolean `prevent_deep_idle` in the same canonical
  file. Missing fields (including in 1.0/1.1 files) mean false; invalid
  types report `preferences.prevent_deep_idle` at `/prevent_deep_idle` with
  "use true or false". It uses the same save/intent/effective-state pattern
  as memory locking. The active note says "Requested: 20 µs"; failures show
  errno and a corrective hint; Off has no status note. This row uses the same
  label/button columns as the others. Its single small caution line says the
  request affects system idle while the engine runs and may increase power,
  heat and fan noise; a second says how to grant write access to
  `/dev/cpu_dma_latency` (a udev rule giving the audio group read/write, and
  membership of that group). Disable or
  engine stop releases only the owned request; transport stop retains it.
  Notes refresh through the existing GUI service flow. No preference test
  opens the actual latency device for writing.
  **Render folder** (1.3): the render dialog's folder choice (§4.8), an
  absolute path saved as the optional string `render_directory` and applied
  to `EngineApp::setRendersDir` at once and on every start. Absent (and in
  1.0–1.2 files) it means the default media folder; anything other than an
  absolute path reports `preferences.render_directory` at
  `/render_directory`.
  **Copy imported audio into project** (1.4): On by default, saved as the
  boolean `copy_imported_media` (absent, and in older files, means true;
  other types report `preferences.copy_imported_media`) and applied to
  `EngineApp::setCopyImportedMedia` at once. A note says the project then
  keeps working if the original file moves or is deleted.



## 7. Visual identity

Quire's own character, not a REAPER clone or a generic dark theme
(`tools/gui/mockups.py` renders mockups of it):

- **Material:** paper and ink — warm neutral grounds (light: off-white
  paper; dark: deep warm graphite, not blue-black), ink text, fine ruled lines
  for grids (the quire: gathered, ruled sheets). **Dark is the default.**
- **Two palettes, one accent each:** *warm* (an orange ink; the default) and
  *ink* (blue). The palette sets the accent, focus and selection; everything
  else is shared. View > switch light / dark and View > switch warm / ink
  palette apply at once and are saved in `<config>/<slug>/appearance.json`;
  `--theme` and `--palette` override them for one run.
- **Type-led hierarchy:** Inter for UI, IBM Plex Mono for every number and
  for notation; few font sizes; labels quiet, values prominent.
- **Strip types as bindings:** Channel blue, Buss amber, Master red, in every
  palette, on the TCP badge, the mixer strip's top edge and the routing
  matrix's marks. Masters carry a heavier spine on the routing marks; in the
  mixer they look like every other strip (the same thin top stripe) and sit
  pinned at its right edge. (Busses' double
  rule from the proposal is not drawn.)
- **Track colours, for organizing:** a small set (red, orange, yellow, green,
  teal, blue, violet, grey; `track.set {color}` also takes "#rrggbb") drawn as
  a stripe down the TCP row's left edge and under the mixer strip's name.
  Picked from the track's right-click menu (Rename, Color…, Delete; the same
  in the TCP and the mixer) or Track > color….
- **Calm by default:** colour reserved for state (armed, clipping, selected,
  agent-changed), meters and waveforms in ink tones; motion only for
  feedback. The TCP keeps its boxed toggles.

## 8. Commands and UI state

Every GUI action is a command or a UI action in the actions list
(`app_actions.h`); the GUI never edits the session any other way. State that
is not the session's (selection, zoom and scroll, panel sizes, screensets,
plug-in editor placements) lives in the UI sidecar (§1.2); `gui.selection`
exposes the selection to agents.

## 9. Performance and benchmarks

- `<slug> --bench gui` opens a generated 64-track session (audio items with
  peaks prebuilt, MIDI items, automation), runs scripted zoom/scroll/drag
  sequences offscreen and reports per-frame CPU time (p50/p95/max), UI-thread
  task durations over 2 ms (each bus pump with its sidecar and editor
  service, each panels' sync pass, each input event; frames are counted on
  their own), idle CPU over 10 s and cold start to first
  interactive frame. Results in `docs/benchmarks/gui.md`; the gates of
  ADR-0005 §5 are acceptance criteria.
- Telemetry for long main-thread tasks in debug builds (a watchdog that
  logs any task over 2 ms with its command name).

## 10. Testing

Unit tests for layout, input routing, damage, theme loading and the peak
pyramid; golden images for widgets and each view in fixed states (offscreen,
tolerance-aware); scripted UI tests that drive views by node id and assert
on the resulting session (through the bus) — the same commands the agent
uses, so a UI test can also be an agent workflow test. TSan for the
worker/UI handoffs. Plugin GUI embedding keeps the template's host probe
tests (`*-linux-host-probe`).

## Platform directory roots (2026-10-14, ADR-0035)

Configuration, data and peak-cache defaults share core-base's paths helper.
Linux retains its XDG/HOME locations. macOS uses Library/Application Support and
Library/Caches; Windows APPDATA and LOCALAPPDATA. User Documents/media defaults
use paths::home(). Product slugs and explicit file overrides remain caller-owned.
This change does not port GUI windowing or plugin-editor embedding.
