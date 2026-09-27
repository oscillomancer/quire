# Status

Updated 2026-09-27 10:10 by the orchestrator. Compact and current: what's done,
in progress, blocked, next. Task IDs are stable (P<phase>.<n>).

## Phase 0 — Reconnaissance and foundations

| ID | Task | State |
|---|---|---|
| P0.1 | Template study → `docs/research/template.md` | done (scout) |
| P0.2 | REAPER study → `docs/research/reaper-ux.md` + RPP evidence | done: files/CLI/scripting (reaper); action inventory via `kbd_enumerateActions`, FX chain, render dialog + wildcards, routing/render matrices, anticipative-FX prefs via GUI (orchestrator, computer-use). Remaining for Phase 5: comp lanes, mouse modifiers, isolation "Run as" in the FX browser |
| P0.3 | Linux audio survey → `docs/research/linux-audio.md`; ADR-0004 | done |
| P0.4 | GUI/windowing evaluation + spike → ADR-0005 | done (scout spike) |
| P0.5 | Scripting decision → ADR-0006 | done |
| P0.6 | Monorepo, CMake presets, format/lint, local CI, license gate | done |
| P0.7 | Living docs: ARCHITECTURE, ADRs 0001–0011, STATUS, HUMAN_REVIEW, THIRD_PARTY | done |
| P0.8 | Design review of ADRs/specs by a second agent | done (scout; `docs/research/review-2026-09-26-design.md`); all critical/major findings resolved in specs/ADRs |

**Phase 0 accepted 2026-09-26:** `tools/ci/clean-clone.sh` builds + tests a
fresh clone offline with one command; `tools/ci/run-all.sh` green on dev, gcc,
asan, ubsan, tsan, rtsan + license + format; ADRs 0004/0005/0006 recorded.

## Phase 1 — Shared core libraries

| ID | Task | Spec | State |
|---|---|---|---|
| P1.1 | core-json (value, parser w/ locations, canonical writer, pointer, merge patch, diff, fuzz) | `specs/core-json.md` A | **done** (json; merged ecd8b7d; orchestrator review fixed 2 bugs). Follow-up: larger RFC 8259 corpus, canonical golden files |
| P1.2 | core-schema (typed binding, validation with hints, units, JSON Schema export, DocumentType) | `specs/core-json.md` B | **done** (json; merged). Follow-up: test every error code's hint text |
| P1.3 | midi (model, canonical order, SMF I/O) | `specs/notation.md` A | **done** (notation; merged 999c0b2). Review by scout pending |
| P1.4 | notation (compiler, decompiler, round-trip law, corpus, fuzz) | `specs/notation.md` B–C | **done** (notation; merged 999c0b2; amendment merged b567154: implicit `voice main`, `bar_after_pipe` hint, generated `skill/generated/notation-reference.md` with a drift test). Review by scout pending |
| P1.5 | core-params (semantic params, units, curves, normalization, text) | `specs/core-params.md` | **done** (orchestrator); reviewed (scout round 2), fixes in 6d62923 |
| P1.6 | core-patch (patch documents, merge, validate, canonical, describe) | `specs/core-patch.md` | **done** (orchestrator); reviewed (scout round 2), fixes in 15b7324 |
| P1.7 | core-analysis: loudness (BS.1770/R128, true peak, LRA) validated against Tech 3341/3342 signals | `specs/core-analysis.md` | **done** (analysis; merged 514cf5d): Tech 3341 cases 1–23 and Tech 3342 LRA 1–4 within tolerance at 44.1/48/96 kHz; histogram and exact modes. Follow-ups: exact-mode overflow flag, stream flush, running-sum drift over hours |
| P1.8 | core-analysis: spectral, harmonic, envelope, stereo, comparison, masking, reports | `specs/core-analysis.md` | **done** (analysis + orchestrator; 363124c): STFT descriptors, YIN pitch, harmonics, envelope, onsets, stereo, compare, profiles, masking; example reports in `docs/specs/examples/analysis/`. analysis-2 (5.0 downmix, hard-pan balance, profile weight validation, speed-up) in progress on p1-analysis-2 |
| P1.9 | core-dsp: oversampling, filters, smoothing, interpolation, denormals | — | **done** (analysis; merged 514cf5d): biquads/bilinear/K-weighting, windows, polyphase upsampler, FFT (pocketfft + RT radix-2), one-pole, denormal guard |
| P1.10 | audio-file: WAV read/write, FLAC/MP3 decode, resampler | `specs/core-analysis.md` §9 | **done** (orchestrator; resampler by analysis, merged 514cf5d) |
| P1.11 | command-bus: registry, descriptors, dispatcher, batch, doc/schema export, example-tests, jobs | `specs/command-bus.md`, `specs/optimizer.md` §4 | **done** (json; merged bd33e44; reviewed by the orchestrator: dispatch/batch/transactions/error paths sound). Follow-up: the meta commands' summaries are terse ("Run commands atomically.") — batch is an MCP tool, give it a real description |

Acceptance: unit tests per library; loudness validated against EBU R128
reference signals; notation round-trips its corpus; example analysis
reports committed in docs.

## Phase 2 — Sandbox host and the first MCP loop

| ID | Task | State |
|---|---|---|
| P2.1 | `specs/semantic-params-extension.md` (CLAP ext `com.suppleaudio.semantic-params/1`) | spec frozen; ABI header in `plugins/common/include/supple/clap/semantic_params.h` |
| P2.2 | host/clap-host: load, instantiate, activate, process with note/param events, state, semantic ext | **done** (orchestrator); reviewed (scout round 2); fixes 15b7324: enforced lifecycle, pending restart/flush requests, event-arena overflow, tail notifications; new `test.edge` fixture |
| P2.3 | probe synth from the template (instrument, CLAP-only offline build, C++20) | stage 1 **done and merged** (11e3892). Stage 2 (semantic params; reusable `plugins/common/cmake/LinkMonorepo.cmake` for ExternalProject plugins) nearly done on p2-probe-2 — paused at the Codex limit (dev + rtsan green, uncommitted) |
| P2.4 | host/sandbox: render notation/MIDI/audio through a plugin offline, analyze, write WAV + report; CLI | **done** (orchestrator; analysis commands 4ae9c89): renderer (147336b + review fixes), `Session` (107f7f4: instances/chain/params/patches/presets/renders/transactions), 16 commands on the bus + `SandboxHost` with NDJSON command log, `quire-sandbox exec`/`registry` CLI, generated `skill/generated/sandbox-*.{md,json}` with a drift test. Examples use the test fixtures until the probe synth is semantic. Missing: `analyze`/`compare`/`masking`/`profile.evaluate` + render `analyze` levels (need core-analysis), audio-file resampling, `--mcp` (needs P2.6) |
| P2.5 | control-protocol: JSON-RPC 2.0 NDJSON over Unix socket | **done** (json; merged 6a04ebf): `libs/control-protocol` server/client, `serve` subcommands in quire-sandbox and quire-engine (model loop on the main thread, SIGINT/SIGTERM), bounded requests/responses with backpressure, socket ownership by lock file + inode; review round 4 findings fixed (98d425f, reviewed by the orchestrator; scout re-review queued) |
| P2.6 | mcp adapter (stdio) generated from the registry; sandbox tools | **done** with P2.5: in-process `--mcp` and the `<slug>-mcp` proxy; capped stdio frames; live proxy integration tests against both binaries |
| P2.7 | scripted end-to-end MCP test + transcript in `docs/demos/` | the fixture-based transcript exists (`docs/demos/mcp-sandbox.txt`); the Orrery session demo is brief json-5 |

## Phase 3 — Stock plugins

| ID | Task | State |
|---|---|---|
| P3.1 | flagship synth "Orrery": architecture + patch format spec | **frozen** for P3.2 (`specs/synth.md` + Amendments); Nate's review (HUMAN_REVIEW #6, #7) can still change it |
| P3.2a | Orrery CLAP core + editor bridge | **done** (dc0366a + this session): `plugins/orrery/clap` (params, state, semantic extension over the structured patch, `orrery-headless.clap`), `EditorBridge` (gesture-bracketed edits to host + engine, structural merges, telemetry, extension hook for the template plugin's GUI), `orrery/edits.h` (structural edits as merge patches: add/remove/retype with route cascades, fx chain, routes, available sources/destinations — every offered route is tested against the codec). **Plugin with editor done** (`plugins/orrery`, template-derived; `Orrery.clap`): signal-flow diagram, modulators, inspector, routes, menus; stereo/mono and 64-bit; tests: editor-input, host-smoke (stereo + mono), semantic conformance, wrapper checks — in every CI preset. Wavetables done (42a98fe). **Loaded and exercised in REAPER 7.80 on this machine** (2026-09-26, release build in ~/.clap): 223 params, MIDI phrase → render at −8.1 dBFS peak, editor embedded (`tools/reaper/load-test.sh Orrery`). Next: clap-validator (P3.10, probe-3), performance |
| P3.2 | synth DSP modules (VA/wavetable/FM/noise/sub osc; ladder/SVF/comb/formant filters; envs, LFOs, step mods; mod matrix; drive; post-FX; poly/unison/glide/MPE) | **core done** (orchestrator, 3663fe4): `plugins/orrery/engine` — 230-param table, patch codec (whole-patch validation, merge, describe, JSON Schema), real-time engine (bit-identical across block sizes, seeded, wait-free program swap), all module types except real wavetables (sine placeholder), FX rack, IIR half-band decimation; 37 tests. CLAP core + headless module in progress. Open: wavetables, ADAA drive, per-stage ladder saturation, noise tilt, aliasing measurements, performance (10.7 % vs the 3 % target for the reference patch) |
| P3.3 | parameter optimizer (CMA-ES, multi-descriptor loss) as a sandbox tool | **done** (orchestrator, aa53962): `libs/optimize` CMA-ES; sandbox `optimize.start` job (reference and profile targets, trajectory term, "auto" parameter selection); end-to-end test recovers a hidden Orrery patch within 10 %. Review round 5 fixes (clone lifecycle on one thread, reference analysis off the model thread, prompt cancel, profile validation/weights, envelope terms, render metadata, thread reaping) → analysis (brief analysis-3) |
| P3.4 | synth evaluation suite (descriptive + reference-matching tasks) and listening queue | draft `specs/eval-harness.md`; model/budget choice queued (HUMAN_REVIEW #8) |
| P3.5 | EQ | planned |
| P3.6 | dynamics (comp/limiter with sidechain, gate/expander, transient shaper) | planned |
| P3.7 | saturation family with oversampling | planned |
| P3.8 | delay (tempo-synced) and algorithmic reverb | planned |
| P3.9 | utility + live analyzer publishing core-analysis data | planned |
| P3.10 | clap-validator built in user space; per-plugin REAPER load test | planned |
| P3.11 | optional REAPER bridge (ReaScript + socket/MCP adapter) | planned |

## Phase 4 — Headless engine (specs drafted early)

Acceptance (build prompt Phase 4), as of 2026-09-27:

| Criterion | State |
|---|---|
| Multi-track session with CLAP plugins in real time at 64-sample buffers without xruns under a defined load | **open**: preliminary B1 (docs/benchmarks/b1-preliminary.md: anticipative ≈96 Orrery tracks vs ≈4 in the callback, paced stand-in device); B1 proper needs the stock EQ/compressor (eq-1) and a run on the device at a forced 64-frame period (HUMAN_REVIEW: forcing the graph quantum affects every PipeWire client) |
| Offline render bit-identical across repeated renders; nulls against real-time capture | **passes** (engine.tests: RT ≡ offline in both modes, B4 determinism for any thread count, with loops, monitoring, automation) |
| Undo/redo fuzz-tested | **passes** (p4-fuzz: history property fuzzer + seeded engine test; project-file fuzzer) |
| A crashing plugin in isolation does not take down the engine | **passes** (P4.8; shared bridge in review) |
| An LLM over MCP builds a small arrangement headlessly and renders it | **passes** (P4.14, engine.mcp-arrangement) |
| Benchmarks vs REAPER recorded in docs/benchmarks/ | **open** (needs B1 proper) |

| ID | Task | State |
|---|---|---|
| P4.0 | `specs/session-model.md` (objects, time map, versions/branches, project file, command catalogue) and `specs/engine.md` (plans, PDC, anticipative processing, isolation, benchmarks) | drafts, used as the basis for the P4 core below (amendment: `fx` is a top-level project collection); design review by reaper/scout still wanted before the command catalogue |
| P4.1 | core-json `compactRows` writer option for bulk arrays in project files | not needed so far: the canonical writer already prints MIDI rows one per line |
| P4.2 | `libs/timeline`: tempo/meter maps, closed-form ramps, bar/beat, pickups | **done** (orchestrator, ba816c6) |
| P4.3 | `host/engine/model`: ids, IdMap, object types + schemas, EditTxn, invariants, diff, project file, VersionStore (undo/redo/coalescing/branches/replay merge with field-level conflicts) | **done** (orchestrator, 4a327ac); review queued (scout-5) |
| P4.4 | `host/engine` offline renderer: plan compile (topological, fixed summation order), MIDI items, audio items (resampled to the plan's rate on load, cached per rate; the real-time engine runs at the device's rate whatever the project's), FX chains via clap-host, mixer (width/volume/pan law/mute, master balance), routes with taps, PDC, mixer automation (track volume/trim/pan/width/mute, route volume/pan/mute; shapes), solo, engine.graph lines; real-time `Engine` on the same plan (RtHandoff plan swap, transport commands over SPSC, instance start/stop on the audio thread, prune after adoption) with a `DummyBackend` (manual / threaded) — offline and real-time output are bit-identical | **done** (orchestrator, 275682b, 52e29cc, b14ce97, fe7ee89). Next: stems; a disk-backed, background media cache (resampling is synchronous at plan compile today) |
| P4.6 | audio backends (ADR-0004): JACK via dlopen | **done** (orchestrator, this session): `JackBackend` (own symbol table, loaded once per path; block-sized callbacks whatever the period; `PIPEWIRE_LATENCY` request, `forceBufferSize` opt-in; xrun/rate-change/shutdown reporting; final zero-frame callback on the JACK thread, 2 s fallback), tested against a stand-in libjack (period/rate changes, xruns, stalls, server shutdown, engine playback, bus playback). Real device (opt-in test): 48 kHz, 128-frame period granted for a 64-frame request, 0 xruns. `engine.start/stop/status`, `transport.play/pause/stop/seek/get`, `quire-engine play FILE`. Open: automatic restart on device change, JACK MIDI ports, deadline-miss xruns, the forced 64-frame period for B1 |
| P4.10 | recording (ADR-0014 phase 1) | **done** (orchestrator, this session): armed tracks' inputs captured on the audio thread into SPSC rings, written by a writer thread to float WAV with periodic header rewrites (crash-readable), placed by play position minus plan + device latency; `track.set {armed, record_input, monitor}`, `transport.record`, takes committed as one version on stop; crash recovery — a sidecar per recording, header repair, `project.recoverable`/`project.recover`, `recoverable` count on `project.get/open`. Open: punch/loop takes, RF64 |
| P4.11 | input monitoring — the live region (ADR-0014 phase 2) | **done** (orchestrator, this session): live classification (monitoring + downstream + master), monitored input in the node's input sum (also while stopped), two executors over one plan — the renderer renders ahead and fills per-frontier-edge rings, the callback runs live nodes at the device block and switches plans at the render horizon; plans reclaimed by generation; per-instance owner tokens. Anticipative output with a monitored track is bit-identical to the in-callback reference; tsan/rtsan clean. A node leaving the live region while playing gets a fresh renderer instance (no dry pass-through); the idle renderer follows the callback while all is live. Open: live multiprocessing, class-change crossfade |
| P4.12 | transport loop, punch, loop recording into takes (ADR-0015) | **done** (orchestrator, this session): loop playback in both modes and with monitoring (wraps mid-block without a gap; stream positions for cross-executor comparisons; held notes released at the wrap; tails ring on), punch applied at commit, one item per loop with a take per pass (newest whole pass active), `transport.loop/punch`, CLAP transport loop flags; epoch, seek and loop published under one sequence lock (the audio thread never waits for a writer; review 13). Mix and bypass changes ramp over 10 ms across plan swaps. Open: loop passes spanning a loop change |
| P4.13 | MIDI input: live monitoring and recording (ADR-0016) | **done** (orchestrator, this session): JACK MIDI input ports per device (optional symbols), Dummy MIDI for tests; MIDI-monitored tracks play live in the callback (device/channel routes); MIDI takes committed as notes/controls per run or loop pass, punch-trimmed; `track.set {record_midi, midi_channel, record_mode}` (midi_overdub merges as a new take). Open: MIDI crash recovery, MIDI outputs |
| P4.14 | Phase 4 acceptance: an arrangement over MCP | **passes** (orchestrator, this session): `tests/integration/mcp_arrangement.py` (CTest `engine.mcp-arrangement`, dev/gcc) — an MCP client builds tempo + three Orrery tracks with patches and notation MIDI + a basic mix in one batch, renders, analyzes, saves, reopens and renders bit-identically; transcript in docs/demos/mcp-arrangement.md |
| P4.9 | plugin-parameter automation | **done** (orchestrator, this session): envelopes on plugin params (semantic keys in physical units, interpolated on the parameter's curve; CLAP ids/names in CLAP values), sample-accurate events every 32 samples and at points, latency-aligned, RT ≡ offline; `env.add/points.set/set/remove/list`. Open: write/touch/latch recording, automation items |
| P4.8 | plugin process isolation (ADR-0013) | **done, dedicated mode** (orchestrator, this session): `<slug>-bridge` + bridged `host::Instance` (control socketpair, memfd data plane, futex, 2 s deadline (lifecycle commands 7 s), pidfd); crash/hang fail only the instance; `fx.set {isolation}`, `fx.restart`, `failed_fx`, `fx.failed`. **Phase 4 acceptance "a crashing test plugin in isolation mode doesn't take down the engine" passes** (engine.tests). Shared mode (probe, p4-shared-bridge): shared fx share one bridge process, kept ready while idle. Open: GUI across processes, B6 benchmark |
| P4.7 | anticipative processing + worker pool (ADR-0012) | **done, first cut** (orchestrator, this session): renderer thread renders the whole graph 200 ms ahead in 1024-frame blocks into an epoch-tagged chunk ring (seek/pause flush by epoch, edits at the horizon without gaps, misses counted); DAG worker pool over plan nodes (bit-identical for any thread count; offline renders use it — B4 test). tsan/rtsan clean. Open: live nodes and live multiprocessing (needs monitoring/recording), B1 capacity benchmark (needs the stock EQ/compressor), futex parking tuning |
| P4.5 | engine command catalogue on the bus (session-model §7) | **core done** (orchestrator, bbb694c, 42b45e2): `EngineApp` (a `cmd::Host`: one version per bus transaction, invocations recorded for replay, plugin catalogue, scratch instances for patch validation, save/open, render) and 32 commands (plus the bus built-ins) — project.get/set/save/open, session.describe, tempo.set, timesig.set, track.add/remove/set/list, item.add/remove/move, midi.write/get, fx.add/remove/set/describe, fx.patch.merge, fx.param.set, render.mix, history.undo/redo/list, branch.create/switch/list/diff/merge/delete (merges replay invocations; field-level conflicts reported). `quire-engine exec`/`registry` CLI and generated `skill/generated/engine-*.{md,json}` with a drift test. Long tail (analyze/compare/masking in the engine, route.*, env.*, marker/region, take.*, item.set/split/duplicate, fx.move, fx.patch.get/set, fx.param.get, plugin.list/scan, time.convert, render.stems) is a Sol task; engine.* and transport.* done with P4.6 |

## Team (Codex workers via Herdr)

Roster + briefs: `~/Projects/quire/.orchestrator/` (git-excluded). A 5 h
Codex window lasts ~50 min of four busy workers; weekly 84 % left (resets
Oct 3). Codex takes well-specified implementation and every review; the
orchestrator does the design and most new subsystems.

- **json** (Sol) — P2.5/P2.6 round-4 fixes on p2-rpc (brief json-4).
- **probe** (Sol) — P2.3 stage 2 done (7534fb5); round-4 fixes (v1 state seed, stepped-value migration), then merge.
- **analysis** (Sol) — analysis-2 finishing on p1-analysis-2; next analysis-3 (optimizer review fixes).
- **scout** (Sol) — reviews: round 3 (Orrery, engine RT, VersionStore: 15 findings, fixed in d1df00e), round 4 (p2-rpc, p2-probe-2), round 5 (optimizer + round-3 audit: 11 findings; 5 and 11 fixed by the orchestrator: backend final-callback shutdown, drive-off message).
- **notation**, **reaper** (Sol) — idle.

Test labels: `slow` (dev/gcc/asan only), `tsan-skip` (AGENTS.md). Local CI
is ~25 min per full run and grows with the optimizer/analysis suites.

Not yet fixed from review round 2 (minor, recorded): describeFlat skips hidden
params in Full detail; core-params formatting of extreme magnitudes (>6
decimals); Rng::uniform extreme endpoints; RtHandoff forced-schedule tests.

## Blocked / waiting on Nate

See `HUMAN_REVIEW.md` (name veto, license choice; FYIs on location, kernel,
fonts). Nothing is blocked.
