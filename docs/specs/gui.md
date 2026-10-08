# The DAW GUI

This spec describes the GUI as built and is updated with it. Decisions it
builds on:
ADR-0005 (SDL3 X11 windows, GL3 + NanoVG, own retained toolkit, performance
gates), ADR-0007 (command bus), ADR-0008 (versions, branches, undo),
ADR-0019 (typed strips), `ARCHITECTURE.md` §3 (threads, event delivery) and
§10, and the workflow research in `docs/research/reaper-ux.md` (the top-40
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
  installed when its job is done); the UI sidecar's periodic writes go to a
  writer thread (exit and project switches write synchronously, after it);
  waveform lookups of media without a content hash read the file's size and
  mtime at most every two seconds and again on each new session version.
- **Event-driven loop.** `SDL_WaitEventTimeout` with the timeout set by the
  nearest deadline: CLAP timers, the bus service tick (`ModelLoop::every`,
  `engine.md` §6), animation frames, meter refresh (30 Hz while audio runs, 0 while
  stopped and silent). Idle with no timers due = no wake-ups.

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
lanes falls back to the pointer. Keyboard zoom anchors at the playhead. Horizontal zoom is continuous;
vertical track height per track and global.
Left trim shifts every take's source offset by the trimmed duration times
its play rate, in the same transaction. Clipboard track offsets are in
channel order; pasting skips envelope rows and reports items dropped for
lack of a target channel. Project replacement clears the item clipboard.
MIDI thumbnails map offset, rate and loop passes
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

Chain list with add (searchable browser over the plugin scan cache;
favourites and stock plugins first; the browser also opens on its own from a
strip's "+", titled with the track and with its own Close), reorder by
dragging a row (an insertion bar; Escape cancels; Alt+Up/Down on a focused
row too; no arrow buttons), bypass vs offline
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
cannot remain open. Each editor registration (an open, or a reopen for a
replaced instance) watches its descriptors under its own serial: readiness
reaches only the registration it was found for, and a poll that overlapped a
change of the watched set is discarded and repeated. Plugin show/hide requests change the parent window's
visibility while retaining the editor and its timer/FD service. Close takes
precedence; when show and hide are coalesced together, hide takes precedence.
Opening an existing hidden editor shows and raises that same window. Each
effect's editor reopens where it was last, at its last size when it is
resizable (a plug-in may adjust the size it is offered); placements are kept
per effect in the UI file's `plugin_windows` section (`{fx id: {x, y, width,
height}}`), so they survive closing the project. A position no longer on any
display (a monitor that is gone) is not used. Commands: `fx.*`, `plugin.*` (scan/describe), `param.*`, `patch.*`.

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
opens. The dialog tracks one render or queue job at a time; a second launch
is refused with a status message while it tracks that job.

### 4.9 Recording

Arm, input and monitor in TCP and mixer; record mode (normal, punch,
loop takes, ADR-0014/0015) in the transport; the record path is shown before
recording starts (`reaper-ux.md` §10).

### 4.10 Command log and history

A dock showing the command log (every command with origin — GUI, agent,
Lua, MCP — arguments, result/error) and the version history per branch
(undo/redo, jump to version, branch list). Agent commands are visibly
attributed.
The history panel refreshes on changes to the project generation, head
version, branch list or current branch name, including branch changes that
leave the head version unchanged.

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
  commands; shows bound keys; runs, binds, adds to toolbar/menu.
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
  click (select, toggle, extend), fade handle (drag, cycle shape), empty-lane
  drag (select, add), automation lane (edit, delete), ruler markers/regions
  (move, remove) and tempo points (toggle ramp) — from each modifier
  combination to a behaviour. The defaults are the built-in gestures;
  `mouse.json` (supple.mouse/1.0, only what differs) overrides them, and
  Edit > Mouse modifiers edits the live map. One context, `wheel.zoom`, is a
  single setting rather than a modifier map: the wheel's zoom anchor
  (`project_cursor`, the default, or `mouse_pointer`).
- **Preferences** (Edit > Preferences…): app-wide settings, never project
  state. The default monitoring of new channels (off, on — the default — or
  auto; `preferences.json`, supple.preferences/1.2, sets
  `EngineApp::defaultMonitor`, so the agents' track.add follows it too), the
  wheel zoom anchor (kept in `mouse.json`), and a way into the mouse
  modifiers. **Audio processing threads** is an advanced live/background
  override: Automatic (default) or a limit per pool including its submitting
  thread; Limit 1 is serial. The menu
  offers 1, 2, 4, 8, 16, 32 and 64; `audio_threads` accepts any integer 1..64,
  with 0 meaning Automatic. Automatic uses the hardware-derived capacity and
  graph-aware participation (engine.md §2.2, ADR-0023/0024). A note says it applies
  on the next engine start; changing it never restarts playback immediately.
  The limit applies independently to each pool, not total process threads;
  offline export keeps its separate thread setting. Version 1.0 files remain readable; missing `audio_threads`
  means 0, and saves write canonical 1.2. Invalid types/ranges return
  `preferences.audio_threads` at `/audio_threads` with a corrective hint;
  a failed save leaves the effective and displayed thread setting unchanged.
  **Lock process memory (mlockall)** is off by default, saved as the boolean
  `lock_memory`. Both 1.0 and 1.1 files remain readable; an absent field means
  false. Other types return `preferences.lock_memory`, `/lock_memory`, and
  "use true or false". The option applies on successful engine start or
  immediately when toggled while running. A failed save preserves the previous
  choice, following the existing audio-thread setter. A failed OS application
  keeps the saved/requested choice On with a failed effective note and errno
  plus a corrective hint. The note distinguishes Off, pending and Locked
  (host process only), refreshed on opening/toggling and the existing GUI
  model-service tick. Cautions explain RAM pressure, sample-library/stack
  failure, bridge exclusion and that disabling/stopping also drops plug-in
  locks (engine.md, Optional host-process controls). Canonical 1.2 saves
  preserve existing monitoring/thread values; no project format changes.


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
