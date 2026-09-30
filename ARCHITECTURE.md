# Quire architecture

Quire is a Linux-first DAW in which an LLM is a first-class operator, plus a
suite of stock plugins designed to be programmed by LLMs, without making the
product worse for the human who uses it directly. Its workflow reference is
REAPER (`docs/research/reaper-ux.md`). This document is the map; the
decisions behind it are in `docs/decisions/` (ADRs) and the contracts in
`docs/specs/`.

## 1. The shape of the system

```
          human (GUI)       Lua scripts        external tools       LLM agents
               │                 │                   │                  │ MCP (stdio)
               │                 │                   │ JSON-RPC 2.0     ▼
               │                 │                   │ (Unix socket)  mcp adapter
               ▼                 ▼                   ▼                  │
        ┌──────────────────────────────────────────────────────────────────────┐
        │  COMMAND BUS  (registry · validation · dispatch · batch/transactions)  │
        │  one typed, versioned, JSON-schema'd command set; events out           │
        └───────────────┬──────────────────────────────────────┬───────────────┘
                        │ edit: pure (Session, Args) → Session  │ query / action
                        ▼                                       ▼
        ┌────────────────────────────────────┐     ┌───────────────────────────┐
        │ SESSION MODEL (model thread)        │     │ services: render, analysis │
        │ immutable versioned tree, stable    │     │ (core-analysis), file I/O, │
        │ IDs, branches, per-branch undo      │     │ plugin scanning, optimizer │
        └──────────────┬─────────────────────┘     └───────────────────────────┘
                       │ compile version → RenderPlan (immutable)
                       │ wait-free pointer swap
                       ▼
        ┌────────────────────────────────────────────────────────────────────┐
        │ ENGINE (RT)  audio callback → graph executor → worker threads        │
        │ anticipative render-ahead for non-live tracks; PDC; automation      │
        │ plugins: CLAP (in-process / bridged / dedicated process), VST3 later │
        └───────────────┬────────────────────────────────────────────────────┘
                        │ AudioBackend: JACK (pipewire-jack) · PipeWire · ALSA · dummy · offline
                        ▼
                     hardware
```

Everything that changes state goes through the command bus. The GUI is a
client of the bus like any other; it has no private mutation path. That
single rule gives undo, replay, audit, macros, scripting and agent control
for free, and guarantees that anything a human can do an agent can do.

## 2. Components

| Layer | Component | Role | Spec / ADR |
|---|---|---|---|
| base | `libs/core-base` | `Error`, `Result<T>`, `SUPPLE_RT`, RT primitives | ADR-0003 |
| data | `libs/core-json` | JSON value, precise-error parser, canonical writer, JSON Pointer, merge patch | ADR-0009, `specs/core-json.md` |
| data | `libs/core-schema` | typed schema DSL: JSON ⇄ C++ binding, validation with hints, JSON Schema export | `specs/core-json.md` |
| params | `libs/core-params` | semantic parameter definitions, units, curves, text I/O, normalization | ADR-0010, `specs/core-params.md` |
| patch | `libs/core-patch` | patch/preset documents: schema, validate, merge, canonical form, prose describe | ADR-0009, `specs/core-patch.md` |
| music | `libs/midi`, `libs/notation` | MIDI model + SMF I/O; the text notation compiler/decompiler | `specs/notation.md` |
| audio | `libs/audio-file`, `libs/core-dsp` | file I/O, resampling; shared DSP blocks | — |
| audio | `libs/console9` | Console 9 encode/decode, fader/pan, deterministic dither; reference null test | ADR-0019 |
| perception | `libs/core-analysis` | loudness (BS.1770/R128), spectral, harmonic, envelope, stereo, comparison, masking; compact reports | `specs/core-analysis.md` |
| control | `libs/command-bus`, `libs/control-protocol`, `mcp/` | registry and dispatch; JSON-RPC over Unix socket; MCP adapter | ADR-0007, `specs/command-bus.md` |
| scripting | `libs/lua-bus` (vendored PUC Lua 5.4) | sandboxed Lua scripts on the bus: generated bindings, `q.batch` transactions, `q.on` subscriptions; `script.run` | ADR-0006 |
| hosting | `host/clap-host` | CLAP hosting incl. the semantic-params extension | `specs/semantic-params-extension.md` |
| sandbox | `host/sandbox` | Phase 2 headless plugin sandbox (library + CLI + MCP) | `specs/sandbox.md` |
| engine | `host/engine`, `host/isolation` | Phase 4 headless DAW engine, plugin bridge processes | ADR-0004, ADR-0008 |
| plugins | `plugins/*` | template-derived stock plugins + shared extension code | ADR-0011 |
| app | `daw/gui`, `daw/agent-panel` | Phase 5 GUI, agent panel (scripting is `libs/lua-bus`) | ADR-0005, ADR-0006 |
| agents | `skill/` | bundled LLM skill: generated reference + guides | Phase 6 |

Dependency direction is strictly downward in that table: libs never depend on
host, daw or plugins; plugins use only header-light libs.

## 3. Threads and real-time safety

- **Audio (RT) thread** — the backend callback. Executes the current
  `RenderPlan`: reads inputs, runs live-path nodes at device buffer size,
  consumes render-ahead buffers for non-live paths, writes outputs, posts
  meters into SPSC rings. Never allocates, frees, locks, blocks, logs, throws,
  or touches files. Entry points are annotated `SUPPLE_RT` and every audio-path
  test also runs under RealtimeSanitizer.
- **Worker threads** — execute graph partitions in parallel (lock-free work
  distribution with atomic dependency counters) and anticipative render-ahead
  for tracks that are not live (not armed / not input-monitored), with larger
  internal blocks, into lock-free ring buffers the RT thread consumes. Same RT
  rules.
- **Model thread** — the only writer of the session. Runs the command bus,
  compiles versions into `RenderPlan`s, reclaims retired plans, services
  plugin main-thread callbacks (CLAP's "main thread" is this thread), owns
  plugin GUIs in-process. In the GUI app it is the UI thread; headless it runs
  an event loop for the control socket and timers.
- **I/O threads** — disk streaming for playback and recording (never drop a
  sample: large preallocated buffers, crash-safe recording files), peak-file
  building, media decoding/resampling caches, analysis jobs, renders.

Model → audio: immutable `RenderPlan` swapped with an atomic pointer;
parameter changes and transport commands via bounded SPSC queues. Audio →
model: two kinds of channel, never confused:

- **Reliable, coalescing** channels for anything that is state: parameter
  changes made inside plugin GUIs, plugin "state dirty" and "latency changed"
  notifications, recording progress. Implemented as atomic latest-value
  slots plus dirty bits (per parameter, per node), so they cannot overflow:
  intermediate values may be coalesced, the final value never lost. The
  model thread drains them and turns them into versions (undo steps).
- **Lossy** channels for telemetry only (meters, scopes, analysis frames):
  SPSC rings that drop the newest item on overflow and count drops.

Nothing ever blocks the RT thread. The one sanctioned RT syscall is a
non-blocking wake (`FUTEX_WAKE` / eventfd write) through a single reviewed
wrapper `supple::rt::wake()` (rtsan-exempted inside it), used to wake worker
threads and plugin bridge processes; RT threads never *wait* on anything
but bounded spins with deadlines.

**Main-thread budget.** The main thread is the UI thread, the model writer
and CLAP's plugin main thread (plugin GUIs require it). It stays responsive
because nothing long runs on it synchronously: edit commands are pure and
fast; `RenderPlan` compilation, rendering, analysis, file I/O, peak building,
plugin scanning and Lua scripts run on worker threads and commit results back
as ordered commands; the target is no synchronous main-thread task over
2 ms, measured in Phase 4/5 benchmarks.

**Event delivery to clients.** Every event carries a per-topic sequence
number. A subscriber whose queue overflows receives one `event.dropped
{topic, from_seq, to_seq}` and re-synchronises with a snapshot query — the
only case where a client reads state instead of receiving it.

## 4. Session model, undo and branches (ADR-0008)

The session is an immutable tree with structural sharing. Each edit command
or batch produces a new **version**; a **branch** is a named ref to a version;
**undo** is per-branch and linear. Objects have short stable IDs (`tr_12`,
`it_40`, `fx_7`) from per-kind counters. Agents work on
`agent/<agent>/<topic>` branches; the human reviews an object-level diff,
A/B-compares offline renders (deterministic, sample-aligned, instant
switching) or live-reconciled graphs, and accepts (fast-forward or replay of
the branch's logged invocations onto `main`, with ID remapping and conflict
reports) or rejects.

The engine never reads the model directly; it runs compiled plans. Plugin
instances are reconciled against model versions: unchanged instances are
kept, changed state is loaded, removed ones are torn down on the model
thread.

## 5. The command bus (ADR-0007)

Commands are declared once in C++ with typed argument structs bound through
core-schema. Each has a dotted name (`track.add`), version, kind
(`query`/`edit`/`action`), docs, argument/result schemas with units and
examples, error codes with hints, and an MCP exposure class. Examples double
as tests. The registry exports JSON Schema from which MCP tools, the skill's
reference, Lua bindings/annotations, the GUI action list and the protocol
method table are generated — and committed, with a drift test.

Agents are expected to use `batch` (atomic, one undo step, labelled). Object
references accept IDs or selectors (`{"name": "Bass"}`), resolved and logged
as IDs. Validation reports every problem at once, each with a JSON Pointer
path and a concrete fix.

Events (`session.changed` with per-object change lists; rate-limited
transport/meter/analysis topics) replace polling.

## 6. Plugins

### Hosting

CLAP first (including `com.suppleaudio.semantic-params`), VST3 second, LV2
later. Isolation modes per plugin instance, REAPER-style: in-process, shared
bridge process, dedicated process. Bridged plugins exchange audio and events with the
engine through shared-memory rings. The engine's RT thread writes block n's
input, wakes the bridge (`rt::wake`, non-blocking) and never waits
unboundedly: in the default *pipelined* mode it consumes block n−1's output
(one block of added, compensated latency); in *synchronous* mode it spins up
to a deadline (a fraction of the block period) and, on a miss, outputs
silence for that node and counts a bridge xrun. A control thread watches the
bridge process; a crash marks the node failed (silence + error event) and the
session survives.

**Runtime latency changes.** CLAP only lets an active plugin change latency
via `request_restart`. The engine then prepares a new plan on a worker thread
(the plugin reactivated with its new latency, all delay-compensation lines
recomputed) while the old plan keeps playing, and swaps at a block boundary
with a short crossfade (default 10 ms) on affected paths. Offline renders are
unaffected (plans are fixed per render). Full contract: Phase 4 engine spec. Plugin GUIs are X11 windows embedded into the DAW's X11
windows (ADR-0005), including from bridge processes (X11 child windows work
across processes).

### Stock plugins (ADR-0011)

Copies of Nate's template, renamed per its checklist, built as external
projects. Each: SDK-free DSP core; a core-params parameter table (physical
units, curves, tags); a core-patch JSON patch format that is the primary
interface; the semantic-params extension for hosts that understand it,
standard CLAP params/state for those that don't; seeded randomness;
real-time and denormal safe; a GUI that reflects the patch structure; a
compact prose `describe()` of the current patch.

Value domains (ADR-0010): physical units on the bus and in patches;
normalized curve positions as the CLAP value for generic hosts; DSP values
inside. core-params owns every conversion.

## 7. Perception (core-analysis)

Offline, deterministic measurement of rendered buffers at full resolution:
loudness (integrated/short-term/momentary LUFS, LRA, true peak), spectral
(centroid, rolloff, flatness, flux, named band energies, smoothed long-term
spectrum), harmonic (f0, partial amplitudes, inharmonicity, HNR), envelope
(attack, decay, crest factor, PLR, transient density), stereo (correlation,
width, balance, mono loss), comparisons (per-descriptor deltas against a
reference or a target profile; masking between two signals). Output is a
compact JSON report (a few hundred tokens by default, more on request) and a
matching one-paragraph text summary. No audio model ever "listens".

## 8. Agents

- **MCP**: in-process (sandbox/engine serve stdio) or proxy (`<slug>-mcp`
  to a running engine's socket). A dozen dedicated tools plus
  `describe`/`exec`/`query` for the long tail.
- **Branches first**: the skill teaches agents to propose on a branch and ask
  the human to listen. Every agent action is in the command log and undo
  history with its origin.
- **Division of labour for sound design**: the LLM picks structure
  (modules, routing, notation); the optimizer (CMA-ES, multi-descriptor
  loss) tunes continuous parameters against a reference or target profile.
- **The skill** (`skill/`): generated reference (commands, plugin schemas,
  notation) + hand-written guides, progressive disclosure, drift-tested.

## 9. Formats (ADR-0009)

Strict JSON with a `"schema": "supple.<type>/<major>.<minor>"` first member,
canonical serialization (byte-identical for identical content), ID-keyed
collections, RFC 7396 merge patches, strict validation with "did you mean"
hints. The MIDI notation is its own line-oriented format with a canonical
form. Project files follow the same rules (schema in the Phase 4 spec).

## 10. GUI (ADR-0005)

An X11 main window (XWayland on Hyprland) so third-party plugin GUIs embed
as X11 children. A custom retained-mode toolkit drawn with NanoVG on GL3,
sharing rendering technology with the plugins' editors; REAPER's workflow
model (arrange, TCP, mixer, FX chains, routing matrix, MIDI editor, render
dialog, actions list with bindable keys and macros, customizable toolbars
and menus, themes) with Quire's own visual identity. UI work never stalls
audio: the UI reads model versions and engine telemetry rings; it cannot
block the RT path. The agent panel talks to OpenAI-compatible endpoints
(including local llama-server) or the Anthropic API and shows branch diffs
with A/B and accept/reject.

## 11. Quality gates

RealtimeSanitizer on audio-path tests (a self-check proves the gate fires);
ASan, UBSan, TSan presets; golden renders with tolerance-aware null tests;
fuzzing of every parser and of command-bus inputs; plugin validators;
benchmarks tracked in `docs/benchmarks/`; the license gate on every build.
Definition of done: code, tests, docs, `STATUS.md`, no license issues, and
review by someone other than the author. Nothing is "done" that was not run.

## 12. Build order

Phase 0 foundations → Phase 1 core libraries → Phase 2 sandbox + MCP loop →
Phase 3 stock plugins (synth first, with the optimizer and evaluation suite)
→ Phase 4 headless engine → Phase 5 GUI → Phase 6 skill and evaluation →
Phase 7 DSP language and Lua ergonomics → Phase 8 hardening and ports. See
`STATUS.md` for the live breakdown.
