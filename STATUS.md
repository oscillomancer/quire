# Status

Updated 2026-10-07. This file describes the present: what works, what is
known not to, and what is planned. Keep it current with each change; the
history is in git.

Quire 0.1.0 runs on Linux (X11 or XWayland, JACK through PipeWire or a JACK
server) and is used for real sessions. Full local CI (`tools/ci/run-all.sh`:
license, format, dev, release, gcc, asan, ubsan, tsan, rtsan) passes on
`main`.

## What works

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
  (ADR-0014); a DAG worker pool; plugin delay compensation (with per-FX
  `pdc: ignore`); parallel FX groups; per-FX oversampling (2/4/8x); input
  FX on armed channels, monitored and recorded; plugins keep
  running while the transport is stopped; smooth plan swaps (10 ms ramps).
- **Strips and routing** (ADR-0019 to ADR-0022): typed channel, buss and
  master strips with Console 9 summing (switchable, level-matched); routes
  with taps, channel maps, mono and polarity; sidechain keys; up to 128
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
  automation items with pools, take envelopes, touch/latch/write recording.
- **Rendering** (`specs/engine.md` §3): mix, stems, regions and the render
  matrix; file-name patterns; loudness or peak normalization with a
  true-peak limiter; WAV (16/24-bit with seeded dither, or float, RF64
  beyond 4 GiB) and FLAC; render jobs and a queue; per-file analysis.
- **Plugin hosting** (ADR-0013, ADR-0017, ADR-0018): CLAP and VST3,
  in-process or isolated in shared or dedicated bridge processes (a crash
  fails only that plugin), supervised scanning with a cache, editors
  embedded in process and from bridges, editor windows that remember their
  place per effect, plugin state saved with the project.
- **GUI** (`specs/gui.md`): arrange view, track panel, mixer, FX chains and
  the plugin browser, routing matrix, MIDI editor, render dialog, history
  dock with branches, actions list with key bindings and macros, editable
  menus, toolbar and mouse modifiers, screensets, two palettes in dark and
  light, preferences.
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

Benchmarks are in `docs/benchmarks/`: B1, 30 Orrery tracks with EQ and
compression at 48 kHz and 64 frames without xruns using anticipation
(`b1.md`); the comparison with REAPER (`b2.md`); GUI cold start and frame
costs (`gui.md`); Console 9 strip cost (`console9-strips.md`).

## Known limits

Engine and hosting:
- Without anticipation, a single Orrery chain can miss deadlines at 64
  frames although average load is low (per-block bursts at chord changes);
  not yet profiled.
- Live nodes run in the device callback on one thread (no live
  multiprocessing).
- There is no pipelined bridge mode, so a bridged plugin on a live
  (monitored) track is passed through with a warning; set its isolation to
  in-process to hear it while monitoring.
- Audio backends: JACK and a dummy backend. No direct PipeWire or ALSA
  backend; no MIDI outputs.
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
- A posted job (`render.add_to_project`) that runs during another batch's
  `job.wait` joins that batch's transaction.

GUI:
- No autoscroll during drags; no resize cursor; plugins can be dropped onto
  mixer strips but not onto track-panel rows.
- Key bindings use only the arrange and global contexts; actions that need
  typed arguments run only from the actions list; macros do not pass their
  arguments to steps.

Plugins:
- Orrery's reference patch uses about 9 % of a core at 48 kHz / 256 frames;
  reaching 3 % needs a structure-of-arrays voice bank.
- Stock plugins are tested as Linux CLAP builds; their VST3 and AUv2
  builds are optional and not covered by CI.

Analysis and scripting:
- Lua values converted to JSON are bounded in depth only, outside the
  script's memory and instruction limits.
- The JSON writer does not validate UTF-8, so a Lua string with invalid
  bytes can produce JSON the parser rejects.
- Schema string lengths are checked in bytes but exported as characters.
- Pitch detection can report a pitch for pure DC; complete stereo
  cancellation reports zero mono loss; file analysis in the sandbox reads
  only the first two channels of a surround file.

Smaller recorded items: `describeFlat` skips hidden parameters in full
detail; core-params formats magnitudes beyond six decimals imprecisely;
`Rng::uniform` at extreme endpoints; forced-schedule tests for `RtHandoff`.

## Planned

- Choose a project license before publishing the repository.
- Profile the 64-frame worst case; a pipelined bridge mode; live
  multiprocessing.
- Track groups, stretch markers, MIDI outputs, a background media cache.
- Lua ergonomics (a script library path, hot reload) and a DSP scripting
  language.
- Orrery's structural performance pass.
- Hardening and ports beyond Linux.
