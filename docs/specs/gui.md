# Phase 5: the DAW GUI

Status: **draft** (orchestrator, 2026-10-01). Decisions it builds on:
ADR-0005 (SDL3 X11 windows, GL3 + NanoVG, own retained toolkit, performance
gates), ADR-0007 (command bus), ADR-0008 (versions, branches, undo),
ADR-0019 (typed strips), `ARCHITECTURE.md` §3 (threads, event delivery) and
§10, and the workflow research in `docs/research/reaper-ux.md` (the top-40
list in §16 is the priority order for workflow coverage).

Acceptance (build prompt Phase 5): Nate can produce a complete track in the
DAW without falling back to REAPER for core tasks; workflow gaps are logged
against `reaper-ux.md` and closed in priority order. Performance (ADR-0005
§5): p95 frame ≤ 8 ms CPU at 1080p with a 64-track session in the arrange
view at any zoom; idle CPU ≈ 0; cold start to an interactive window
< 300 ms on this machine; no UI work can stall audio.

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
  measured (§9).
- **Event-driven loop.** `SDL_WaitEventTimeout` with the timeout set by the
  nearest deadline: CLAP timers, the bus service tick (`ModelLoop::every`,
  P4.20), animation frames, meter refresh (30 Hz while audio runs, 0 while
  stopped and silent). Idle with no timers due = no wake-ups.

### 1.1 Observing the model

Views observe the session through the bus's event stream (per-topic
sequence numbers; `event.dropped` → resnapshot, `ARCHITECTURE.md` §3). Each
view keeps a **view model** keyed by stable object ids (`tr_12`, `it_7`…)
and, on a version change, diffs the objects it shows (the model's
`diff(versionA, versionB)` already exists for ADR-0008) and invalidates only
the affected nodes. A view never caches pixels across versions without a
key that includes the object's content revision. The arrange view uses the
version store's change list for adjacent edits and undo; branch jumps compare
shared object identities. Track geometry changes rebuild the rows; other
edits update affected controls, item indexes and old/new item footprints.
Unrelated FX parameter edits preserve TCP control/capture identity and do
not rebuild or repaint the arrange view. Envelope glyphs stay within their
lane or take's item.

### 1.2 UI state is not session state

Zoom, scroll, selection, window layout, open editors, docking and
screensets live in a separate per-project UI file (`<name>.ui.json`,
canonical JSON, never in the project's diff) plus per-user settings
(keymaps, toolbars, theme) under the user config directory
(`$XDG_CONFIG_HOME/<slug>/`). Selection is exposed read-only to agents
(`gui.selection` query) because "the selected items" is useful context;
agents never depend on it for correctness.

## 2. Toolkit (`daw/gui/ui`)

A small retained-mode toolkit, ours (ADR-0005 §4). Not a general framework:
only what the DAW needs, designed for virtualized, data-heavy views.

- **Nodes.** A tree of `Node`s (id, role, bounds, children, flags). Roles
  (`button`, `slider`, `list`, `row`, `track`, `item`, `text-field`…) are
  semantic: they drive keyboard behaviour, UI tests and, later, AT-SPI
  accessibility (Phase 8). Custom-drawn views (arrange, piano roll) are one
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
  inline).
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
  tokens.
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

Per strip: name (rename in place), **strip-type badge** (Channel / Buss /
Master: colour, shape and label differ — clear at a glance, ADR-0019),
mute/solo, arm, input and monitor (channels only), volume fader and pan
(Console 9 Channel's controls on channels and busses; plain gain and
balance on masters; the fader's dB scale comes from `libs/console9`'s
mapping), FX button (chain state: count, bypassed, offline), I/O button
(drag onto another strip to create a route; drop rules follow
`route.strip_type`, invalid targets show why before the drop), meters.
Folder/buss parentage shows as indentation and a parent bar. Under each
parent the channels are listed first, then the busses (each in the parent's own
child order, `views::displayChildren`), so a buss never sits between two
channels; the model's child order is not changed (the mixer groups by type
too, §4.4). Dragging an empty part of a row (not a
button) reorders the track among its siblings *of the same type* under the same
parent: an insertion marker shows the drop, Escape cancels, and the release is
one `track.move {track, index}` (`views::reorderIndex` computes the index;
no-op drops commit nothing). A buss cannot be dropped between channels, and
a track does not change parent by dragging. Adding a strip
offers exactly **Channel, Buss or Master** (`track.add {type}`).
Commands: `track.*`, `route.*`, `fx.*` (bypass/offline), transport.

### 4.3 Arrange view

Ruler (bars/beats and time; tempo and meter lanes; markers and regions),
virtualized track rows (only visible rows exist), items with waveforms or
MIDI, take lanes, fades and crossfades, automation lanes (volume/pan/mute
on strips; plugin parameters), time selection, snapping grid, playhead with
follow modes. On every version change, deleted track and item IDs are
pruned from transient selection; undo restores objects, not old selection.
MIDI editor selection clears on external version changes, and a gesture
cannot commit against a version newer than the one where it started.

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
lanes falls back to the pointer. Keyboard zoom anchors at the playhead. Horizontal zoom is continuous;
vertical track height per track and global.
Left trim shifts every take's source offset by the trimmed duration times
its play rate, in the same transaction. Clipboard track offsets are in
channel order; pasting skips envelope rows and reports items dropped for
lack of a target channel. MIDI thumbnails map offset, rate and loop passes
as playback does. Fade shapes and curve bends share playback's evaluator.
Automation drawing samples only visible time and bounds loop glyph work;
plugin automation interpolates in the playback parameter domain (ADR-0010).
Commands: `item.*`, `take.*`, `fade.*` (as named in session-model §7),
`envelope.*`, `marker.*`, `region.*`, `tempo.*`, `transport.*`.

### 4.4 Mixer

Strips in track order grouped by type: channels, busses, masters (several
masters are allowed, ADR-0019), each with the type's visual language; FX
slots, sends (level/pan/tap, with "follow" pan shown), input/arm/monitor on
channels, fader + pan + meters, Console 9 on/off as a project toggle with its
level-matched behaviour explained in its tooltip. Meters update only when
their entire ancestor chain is visible, and clear peak hold on reopening.

### 4.5 FX chain and plugins

Chain list with add (searchable browser over the plugin scan cache;
favourites and stock plugins first), reorder by drag, bypass vs offline
(distinct, `reaper-ux.md` addendum), wet/dry, per-FX latency and CPU, preset
menu, and the plugin's own GUI (embedded X11 child of an SDL window,
ADR-0005 §2, `clap.timer-support`, resize negotiated from observed
geometry). For plugins without a GUI — and always as a second tab — a
**semantic parameter panel** generated from the plugin's semantic schema
(units, ranges, enums, groups): the same description an agent reads, so the
human and the agent see the same names and units. Stock plugin editors are
the template's Pugl editors. Latency badges refresh after effect model edits,
host replacement and live latency changes. File-descriptor service startup
and poll failures are reported; editors requiring unavailable FD service
cannot remain open. Plugin show/hide requests change the parent window's
visibility while retaining the editor and its timer/FD service. Close takes
precedence; when show and hide are coalesced together, hide takes precedence.
Opening an existing hidden editor shows and raises that same window.
Commands: `fx.*`, `plugin.*` (scan/describe), `param.*`, `patch.*`.

### 4.6 Routing view and matrix

One grid (`reaper-ux.md` addendum 2): sources (strips in hierarchy, hardware
inputs, MIDI inputs) × destinations (parent, strips, plugin keys, hardware
outputs); a cell creates/edits a route (tap, channels, level, pan/follow,
mute, invert, mono); cells that `route.strip_type` would reject are drawn
disabled with the rule as their tooltip; record-input assignments appear in
the same grid. A graph view (nodes and edges, PDC per node) is P1.
Matrix updates clamp scroll to the current endpoint range; paint uses
endpoint-pair indexes for send and cycle rules.
Commands: `route.*`, `track.set {record_input}`.

### 4.7 MIDI editor

Piano roll (instanced notes; draw, select, move, resize, velocity drag,
quantize, transpose), velocity and CC lanes, and a **notation panel**: the
item's notation text (`libs/notation`) beside the roll, editable with live
compile diagnostics (the compiler's located errors and hints); edits in
either side update the other through `item.*` commands. This is the
LLM-native differentiator: the same text an agent writes is visible and
editable by the human. Event list view is P1.

Native file-dialog completions check owner lifetime before posting and again
on the UI thread; outstanding dialogs may safely outlive shutdown.

### 4.8 Render dialog

A front end to `render.*` (P4.15): sources (master, stems, selected
items/regions, region matrix), bounds, tail, output pattern (the typed
variables of the render schema) with a **live dry-run preview** of the
resolved files, formats (primary + secondary), normalization/limiting,
add-to-project, queue. Every option maps to a schema field; nothing is
GUI-only. A nonempty invalid numeric field blocks preview, queue insertion
and rendering with a field diagnostic. Cancelled jobs remain labelled
cancelled and retain any partial result files returned by the engine.

### 4.9 Recording

Arm, input and monitor in TCP and mixer; record mode (normal, punch,
loop takes, ADR-0014/0015) in the transport; the record path is shown before
recording starts (`reaper-ux.md` §10).

### 4.10 Command log and history

A dock showing the command log (every command with origin — GUI, agent,
Lua, MCP — arguments, result/error) and the version history per branch
(undo/redo, jump to version, branch list). Agent commands are visibly
attributed.

## 5. Agent panel (`daw/agent-panel`)

- **Endpoints:** any OpenAI-compatible chat endpoint (including a local
  llama-server) or the Anthropic API; base URL, model and key (key stored
  in the user's keyring or a 0600 file, never in the project). Requests run
  on a worker thread; the UI streams tokens.
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
  commands; shows bound keys; runs, binds, adds to toolbar/menu.
- **Keymaps:** per context section (global, arrange, MIDI editor, mixer,
  text field), chords allowed, conflicts shown in the editor; stored as
  canonical JSON in user config; defaults close to REAPER's common keys
  where they are not proprietary assets (key choices are not copyrightable
  expression; no REAPER files are copied).
- **Macros:** a sequence of actions (or a Lua script) run as one batch (one
  undo step), with typed prompts.
- **Toolbars and menus:** built from actions; customizable; stored in user
  config (`menus.json`, supple.menus/1.0). Edited in the app: Ctrl+T in the
  actions list puts the selected action on the toolbar (or takes it off),
  Ctrl+M adds it to a menu or removes it; a toolbar button's right-click menu
  moves or removes it, or resets menus and toolbar.
- **Mouse modifiers:** a map per context of the arrange view — item drag
  (move, copy, slip; each also off the grid), item edge (trim, stretch), item
  click (select, toggle, extend), fade handle (drag, cycle shape), empty-lane
  drag (select, add), automation lane (edit, delete), ruler markers/regions
  (move, remove) and tempo points (toggle ramp) — from each modifier
  combination to a behaviour. The defaults are the built-in gestures;
  `mouse.json` (supple.mouse/1.0, only what differs) overrides them, and
  Edit > Mouse modifiers edits the live map. One context, `wheel.zoom`, is a
  single setting rather than a modifier map: the wheel's zoom anchor
  (`project_cursor`, the default, or `mouse_pointer`).

## 7. Visual identity

Quire's own character, not a REAPER clone or a generic dark theme. Reviewed by
Nate on rendered mockups (2026-10-02, HUMAN_REVIEW #17;
`tools/gui/mockups.py` renders them):

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
  matrix's marks. Masters carry a heavier spine (the mixer strip's left edge,
  the routing marks) and sit pinned at the mixer's right edge. (Busses' double
  rule from the proposal is not drawn.)
- **Track colours, for organizing:** a small set (red, orange, yellow, green,
  teal, blue, violet, grey; `track.set {color}` also takes "#rrggbb") drawn as
  a stripe down the TCP row's left edge and under the mixer strip's name.
  Picked from the track's right-click menu (Rename, Color…, Delete; the same
  in the TCP and the mixer) or Track > color….
- **Calm by default:** colour reserved for state (armed, clipping, selected,
  agent-changed), meters and waveforms in ink tones; motion only for
  feedback. The TCP keeps its boxed toggles (Nate: no quieter variant).

## 8. Commands the GUI will need (gap list, to verify against the registry)

`gui.selection` (query, §1.2); peak-pyramid jobs and events (`media.peaks`);
plugin scan cache browsing (`plugin.list` with categories/favourites);
transport follow/scroll hints (UI-only, not commands); A/B version
switching for the agent panel (`transport.audition {version}` or an engine
setting); envelope point edit commands at the granularity gestures need.
Each is designed when its view starts, against session-model §7.

## 9. Performance and benchmarks

- `<slug> --bench gui` opens a generated 64-track session (audio items with
  peaks prebuilt, MIDI items, automation), runs scripted zoom/scroll/drag
  sequences offscreen and reports per-frame CPU time (p50/p95/max), UI-thread
  task durations over 2 ms, idle CPU over 10 s and cold start to first
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

## 11. Plan

Ordered; each task is a self-contained brief with tests. The orchestrator
builds P5.1–P5.3 (foundations, architecture-heavy); workers take views.

| ID | Task | Depends |
|---|---|---|
| P5.1 | App skeleton: `<slug>` binary, SDL3 X11 window, GL3 context, NanoVG, event loop with deadlines, in-process `EngineApp`, offscreen mode, cold-start benchmark | — |
| P5.2 | Toolkit core: nodes, layout, input/focus, damage + layers, text, theme tokens, golden-image + scripted-UI test harness | P5.1 |
| P5.3 | Bulk renderers: peak pyramid (worker builder, disk cache), waveform layer, instanced MIDI notes, meters from telemetry; 64-track perf bench | P5.2 |
| P5.4 | Arrange view, read-only: ruler, tempo/markers, virtualized rows, items, playhead, zoom/scroll | P5.3 |
| P5.5 | TCP + strip-type UI + transport bar | P5.4 |
| P5.6 | Arrange editing gestures (P0 list 8–13) via commands, one undo step per gesture | P5.5 |
| P5.7 | Mixer | P5.5 |
| P5.8 | FX chain, plugin browser, X11 plugin GUI embedding, semantic parameter panel | P5.5 |
| P5.9 | Routing matrix | P5.5 |
| P5.10 | Actions list, keymaps, macros, toolbars/menus | P5.2 |
| P5.11 | MIDI editor + notation panel | P5.6 |
| P5.12 | Render dialog with dry-run preview | P5.5 |
| P5.13 | Recording UI | P5.5 |
| P5.14 | Automation lanes and modes | P5.6 |
| P5.15 | Agent panel (endpoints, branch diff, A/B, accept/reject, listening cards) | P5.10 |
| P5.16 | Visual identity pass after Nate's mockup review | P5.7 |
| P5.17 | Performance gates and the workflow-gap log against `reaper-ux.md` | all |
