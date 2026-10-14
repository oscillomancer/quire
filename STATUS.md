# Status

Updated 2026-10-14. This file describes the present: what works, what is
known not to, and what is planned. Keep it current with each change; the
history is in git.

Quire 0.1.0 runs on Linux (X11 or XWayland, JACK through PipeWire or a JACK
server) and is used for real sessions. Full local CI (`tools/ci/run-all.sh`:
license, format, dev, release, gcc, asan, ubsan, tsan, rtsan) passes on
`main`.

The port branch has on-demand Linux/macOS/Windows CI and portable presets
(ADR-0034). Linux dev passes all 137 tests including the 12 CLAP validators;
macOS and Windows pass their portable tests. The macOS preset uses AppleClang
and system libc++; shared float/double parsing uses vendored fast_float (MIT)
on all platforms, including the suite's self-contained source exports.
Non-Linux host, engine,
control/MCP, GUI and plugin wrappers remain gated, and Windows audio-file
I/O remains gated. Portable libraries, the session model, stock DSP cores
and the script suite build independently of those services; see
CONTRIBUTING.md for the complete gate list. External Windows readers that
deny delete sharing prevent atomic file replacement until closed.

## What works

- **Licensing** (ADR-0033): DAW code is GPL-3.0-or-later; stock plugins
  and their shared libraries are MIT.

- **Session and history** (`specs/session-model.md`, ADR-0008): immutable
  versions with stable ids, per-branch undo, branches with object-level diff
  and merge by replay (field-level conflicts, id remapping), A/B renders of
  two branches, a command log with origins, a history budget that squashes
  the oldest steps. Project format `supple.project/2.0` with migration from
  1.x, autosave, crash recovery of recordings, a per-project UI sidecar.
- **Command bus** (`specs/command-bus.md`, ADR-0007): about 120 typed
  commands with JSON Schema, units, examples run as tests and structured
  errors; `batch` transactions; events; the JSON-RPC socket; the MCP adapter
  (in process or as a proxy to the running app); Lua scripts on the bus
  (ADR-0006). The skill's command reference is generated and drift-tested.
- **Engine** (`specs/engine.md`): deterministic offline renders with
  real-time output bit-identical to them; anticipative rendering for
  non-live tracks (ADR-0012) and a live region for monitored tracks
  (ADR-0014); a DAG worker pool with graph-bounded participation and idle
  parking (ADR-0023), a separate live callback fork-join pool (ADR-0024)
  with FIFO JACK helpers, a claim-based lifetime join that does not wait for
  unclaimed helpers, and an Automatic/limit audio-thread preference;
  an opt-in host-process memory-lock preference with effective status and
  actionable OS failures (bridges excluded; cleanup also drops plug-in locks);
  a separate default-off 20 µs CPU idle-latency request with close-based
  cleanup and permission diagnostics (real-device validation is user-only);
  audio threads named by role (`audio-render`, `audio-bg-N` and others);
  the renderer, background helpers and bridge audio threads at SCHED_FIFO 10
  below a real-time callback when permitted, with a warning and normal
  scheduling otherwise, reported per thread class in `engine.status`
  (ADR-0026);
  plugin delay compensation (with per-FX
  `pdc: ignore`); parallel FX groups; per-FX oversampling (2/4/8x); input
  FX on armed channels, monitored and recorded; plugins keep
  running while the transport is stopped; renderer deadline/event waits
  (ADR-0028); headless main-thread service driven by notifications and
  deadlines (ADR-0030), including MIDI controls, plug-in subscriptions,
  render-ahead output consumption, jobs, restart and autosave; smooth plan swaps (10 ms ramps);
  every node, plug-ins and bridged plug-ins included, runs with FTZ/DAZ in
  every executor, and owned audio threads prefault their stacks (ADR-0025).
- **Strips and routing** (ADR-0019 to ADR-0022): typed channel, buss and
  master strips with Console 9 summing (switchable, level-matched); routes
  with taps, channel maps, mono and polarity; sidechain keys (an unrouted
  key deactivates a plug-in's key port or bus, so it can key from its own
  input); up to 128
  channels per strip with plugin pins and multi-output instruments; strip
  polarity and mono; the routing matrix.
- **Recording**: armed inputs to crash-readable WAV, input monitoring (needs
  the arm), MIDI monitoring and recording with overdub, loop recording into
  takes, punch, count-in, metronome.
- **Editing**: items, takes, comping lanes, fades and crossfades with
  shapes, time stretch and pitch (Signalsmith Stretch), markers, regions,
  tempo and meter maps, the item clipboard, a piano roll with velocity and
  CC lanes, an event list and a notation panel.
- **Automation**: envelopes on strip, route and plugin parameters,
  automation items with pools, take envelopes, touch/latch/write recording;
  automation points selected (click, Ctrl+click, or the arrange marquee,
  which takes items and points on any track at once) and moved, deleted,
  copied, pasted and reshaped together, across lanes, from the lane's menu or
  the keyboard.
- **Rendering** (`specs/engine.md` §3): mix, stems, regions and the render
  matrix; file-name patterns; loudness or peak normalization with a
  true-peak limiter; WAV (16/24-bit with seeded dither, or float, RF64
  beyond 4 GiB) and FLAC; render jobs and a queue; per-file analysis.
- **Plugin hosting** (ADR-0013, ADR-0017, ADR-0018): CLAP and VST3,
  in-process or isolated in shared or dedicated bridge processes (a crash
  fails only that plugin), supervised scanning with a cache, editors
  embedded in process and from bridges, editor windows that remember their
  place per effect, plugin state saved with the project. Bridge main loops
  park until socket/descriptor/request activity or the nearest plug-in timer;
  CLAP/VST3 requests use core-base's audio-safe coalesced wake relay
  (ADR-0029), and timers run without an open editor; remote changes/death
  wake the app without editor or audio idle polling (ADR-0031).
- **GUI** (`specs/gui.md`): arrange view, track panel, mixer, FX chains and
  the plugin browser, routing matrix, MIDI editor, render dialog, history
  dock with branches, actions list with key bindings and macros, editable
  menus, toolbar and mouse modifiers, screensets, two palettes in dark and
  light, preferences. Projects and renders default to
  `~/Documents/<slug>-media` (the render folder is choosable), each project
  in a folder of its own with its own media; imports are copied into the
  project by default, and Save as into another folder copies the media. FX
  chain presets and project templates (with a copy of their audio) are
  saved, loaded and deleted in the app. Takes: show all takes in lanes,
  explode onto tracks or along the track, delete one, Ctrl-drag one out, and
  render an item through its FX (MIDI to audio) in place or as a new take.
  The FX chain selects several effects (Ctrl/Shift+click) to bypass, offline
  or remove together; a double click opens an effect's editor. The window
  waits on app/local plug-in/ModelLoop notifications and demand-driven
  deadlines; stopped transport and stationary meters have no refresh timer.
  Input/commands coalesce UI sidecar checks, and writer completion wakes main.
- **MIDI learn** (ADR-0027): any plug-in parameter (the FX window's MIDI
  button, or `fx.learn.set` for agents) and any action (Ctrl+L in the
  actions list) learns a controller by moving it: CC, note, pitch bend or
  program change, absolute, three relative encoder modes or toggle, soft
  takeover, and "only when selected / focused / visible" for parameters.
  Parameter bindings are saved with the project and chain presets; action
  bindings in `controls.json`. "(MIDI CC)" actions drive the selected tracks'
  volume and pan, the master's volume, a beat jog and the last touched
  plug-in parameter.
- **Agents**: the MCP server for the running app or a headless engine; the
  agent panel (Anthropic API or OpenAI-compatible endpoints, local servers
  included) working on `agent/...` branches with diff, A/B and
  accept/reject; the bundled skill (`skill/`).
- **Analysis and sound design**: `core-analysis` (BS.1770/R128 loudness
  validated against EBU Tech 3341/3342, spectral, pitch, envelope, stereo,
  comparison, masking); the CMA-ES patch optimizer (`specs/optimizer.md`);
  the LLM evaluation harness (`tools/eval-harness`).
- **Stock plugins** (ADR-0011): Orrery, Facet, Ballast, Sluice, Flint, Kiln,
  Echelon, Nave, Plumb and Gauge, each passing clap-validator, a reset
  determinism check and an unload test (all but Nave also pass the headless
  REAPER check); JSON patches and the semantic-parameters extension.

- **DSP scripting** (ADR-0032): plugins written in a checked subset of
  C++, run by a JIT player plugin, exported as plugins or C++ projects. The
  compiler and runtime exist (`plugins/suite`, tested against the same scripts
  compiled as C++). So does the player (`plugins/suite/player`, built from
  `plugins/template`, staged with the stock plugins): one plugin per script
  in the script library, with the semantic-params extension, reloading a
  script when its file changes. `dsp.write`, `dsp.check`, `dsp.get` and
  `dsp.list` write and compile scripts (the app's library is
  `<data>/<slug>/scripts`, shared with the sandbox), `dsp.test` measures
  one with standard signals (and, when asked, parameter automation, a timed
  sidechain key or a note sequence), and a script that compiles is loaded
  and set like a stock plugin (`skill/guides/dsp-workflow.md`).
  `fx.describe` and the sandbox's `plugin.describe` report a live
  instance's loop-budget stops, out-of-range indices and failed reloads as
  structured problems. Scripts declare read-only factory patches
  (`factoryPatches(Patches&)`), listed and applied with `fx.patch.list` and
  `fx.patch.set {name}` or the sandbox's `preset.list/load {source:
  "factory"}`, and offered by exports' preset bar and hosts' preset
  browsers. `dsp.export` makes a script a CLAP plugin for any host (a copy
  of the player with the script appended, which offers that one plugin), or
  a C++ project of the plugin template that builds the same plugin, bit for
  bit, with the script compiled as C++ (every example's export is checked
  against the player, for semantic conformance and factory patches, and
  passes its own tests). Effects and instruments
  (voices the runtime manages, CLAP note expressions and per-note
  modulation; no note effects); samples and impulse responses
  as files a patch names (loaded whole, no streaming from disk); spectral
  effects (an STFT), oversampling and sidechains. A script may draw its
  own editor (`editor()` for controls, `draw()` on an HTML-canvas-style
  `Canvas`, `pointer()` to drag and click what it draws, editing
  parameters as a user would, `Shared` members for what the audio code
  shows it); one that does not gets the automatic editor. `dsp.preview`
  draws a plugin's editor to a PNG without a window (EGL, Linux only; the
  editor-picture extension), after clicks, drags and wheel turns it is
  given, and says what they edited. Plugin-file exports are for Linux
  only.

Benchmarks are in `docs/benchmarks/`: B1, 30 Orrery tracks with EQ and
compression at 48 kHz and 64 frames without xruns using anticipation
(`b1.md`); the comparison with REAPER (`b2.md`); GUI cold start and frame
costs (`gui.md`); Console 9 strip cost (`console9-strips.md`). The benchmark
harness also supports 64/128-frame requests, installed third-party effects,
whole-callback/render-job tails and an observed ring low-water mark. Timing is
bench-only; synthetic deadline counts and server xruns are separate. Historical
capacity reports retain their original revision/settings.

## Known limits

Engine and hosting:
- Without anticipation, a single Orrery chain can miss deadlines at 64
  frames although average load is low (per-block bursts at chord changes);
  the harness now exposes callback tails for further profiling.
- Live multiprocessing requires at least two usable CPUs and successful
  helper scheduling; otherwise the engine reports a warning and runs serially.
  A serial plug-in chain cannot benefit, and a stalled helper that has claimed
  a job stalls its cycle. Wake/scheduling overhead remains on light graphs;
  there is no adaptive cost threshold.
- The FTZ/DAZ policy exists on x86-64 (SSE2) only; plug-in lifecycle calls
  run in the thread's own floating-point environment.
- Real-time background threads need `RLIMIT_RTPRIO` ≥ 1, an unlimited
  `RLIMIT_RTTIME` and a pool that leaves two allowed CPUs free; without them
  the renderer and pool run at normal priority (with a warning) and compete
  with desktop load.
- A bridged plug-in's audio thread that cannot take the engine's background
  policy stays at normal priority with no warning or status: nothing reports
  it to the host. The bridge inherits the host's limits, so this needs a
  limit or policy change made to the bridge process from outside; such a
  thread's plug-in can then fall behind under load like an unelevated
  renderer.
- There is no pipelined bridge mode, so a bridged plugin on a live
  (monitored) track is passed through with a warning; set its isolation to
  in-process to hear it while monitoring.
- Audio backends: JACK and a dummy backend. No direct PipeWire or ALSA
  backend; no MIDI outputs.
- MIDI learn has no OSC, no "arm envelope" option, and no learn directly on
  track faders (the "(MIDI CC)" actions cover the selected tracks). The last
  touched parameter misses edits in plug-in editors that report no gestures
  or outputs. Every MIDI input is heard as a control;
  there is no per-device "control only" setting.
- Plugins that change state without notifying the host (LSP, for one) have
  that state captured only when the project is saved: no undo step and no
  dirty mark.
- Stopped-transport audio from tracks that are not live (a plugin's own
  preview button, for example) is not heard in anticipative mode unless the
  track is armed and monitored.
- Multi-output plugins: main output ports wider than stereo are not used,
  and there are no per-output meters.
- Overlapping gestures on a plugin without semantic parameters share one
  state blob.
- The engine does not read plugin telemetry (Gauge's measurements reach its
  own editor and agents through the extension only).
- FX containers are not implemented; parallel FX run in series on pinned
  stages and strips with auxiliary channels, and oversampling is not
  available to plug-ins with auxiliary outputs.
- A bridged plug-in's tail is read once when a sandbox render starts, so a
  tail it changes during that render is not followed (in-process plug-ins'
  are).

Session and editing:
- Take `stretch_markers` are stored but not applied; stretching and
  resampling run synchronously when a plan is compiled.
- Track groups are preserved in project files but not implemented.
- Loop recording passes that span a change of the loop range are not
  handled; MIDI recordings have no crash recovery.
- `fx.copy` does not copy envelopes or key routes; take FX cannot be keyed.
- The history budget's size estimate walks shared payloads per version, and
  squashed steps keep large invocation arguments.

GUI:
- No autoscroll during drags; no resize cursor; plugins can be dropped onto
  mixer strips but not onto track-panel rows.

Plugins:
- DSP source exports require a visible, automatable parameter with a plain
  name for their host tests and do not retain the JIT's loop budget (a loop
  that never ends hangs a native export). Exports include declared factory
  patches, but do not package external audio files or the patch of a
  currently loaded instance. Preview requires an editor build and EGL on
  Linux.
- Scripts need a JIT: there is no fallback for hosts that forbid executable
  memory (MIR's interpreter is a test backend, not real-time safe).
- A script save that redefines the plugin while an editor drag is under way
  can leave that gesture open in the host until it rescans.
- Orrery's reference patch uses about 9 % of a core at 48 kHz / 256 frames;
  reaching 3 % needs a structure-of-arrays voice bank.
- Stock plugins are tested as Linux CLAP builds; their VST3 and AUv2
  builds are optional and not covered by CI.

Smaller recorded items: `describeFlat` skips hidden parameters in full
detail; core-params formats magnitudes beyond six decimals imprecisely;
`Rng::uniform` at extreme endpoints; forced-schedule tests for `RtHandoff`.

## Planned

- Profile the 64-frame worst case; a pipelined bridge mode.
- Track groups, stretch markers, MIDI outputs, a background media cache.
- Lua ergonomics (a script library path, hot reload).
- Orrery's structural performance pass.
- Hardening and ports beyond Linux.
