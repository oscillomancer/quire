# Status

Updated 2026-09-26 18:50 by the orchestrator. Compact and current: what's done,
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
| P2.5 | control-protocol: JSON-RPC 2.0 NDJSON over Unix socket | implemented on p2-rpc (json; 76dbcad, full CI green); review round 4 found no process serving the socket, silent drop of >4 MiB responses, no inbound backpressure, a bind race — fixing now (brief json-4: `serve` subcommands, size/backpressure limits, stale-socket handling) |
| P2.6 | mcp adapter (stdio) generated from the registry; sandbox tools | implemented with P2.5 on p2-rpc (in-process `--mcp` and the `<slug>-mcp` proxy); stdio line cap and error codes in json-4 |
| P2.7 | scripted end-to-end MCP test + transcript in `docs/demos/` | planned |

## Phase 3 — Stock plugins

| ID | Task | State |
|---|---|---|
| P3.1 | flagship synth "Orrery": architecture + patch format spec | **frozen** for P3.2 (`specs/synth.md` + Amendments); Nate's review (HUMAN_REVIEW #6, #7) can still change it |
| P3.2a | Orrery CLAP core + editor bridge | **done** (dc0366a + this session): `plugins/orrery/clap` (params, state, semantic extension over the structured patch, `orrery-headless.clap`), `EditorBridge` (gesture-bracketed edits to host + engine, structural merges, telemetry, extension hook for the template plugin's GUI), `orrery/edits.h` (structural edits as merge patches: add/remove/retype with route cascades, fx chain, routes, available sources/destinations — every offered route is tested against the codec). Next: the template-derived plugin with the editor (`plugins/orrery/plugin/gui/Editor.*` drafted: signal-flow diagram, modulators, inspector, routes, menus), clap-validator, REAPER load |
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

| ID | Task | State |
|---|---|---|
| P4.0 | `specs/session-model.md` (objects, time map, versions/branches, project file, command catalogue) and `specs/engine.md` (plans, PDC, anticipative processing, isolation, benchmarks) | drafts, used as the basis for the P4 core below (amendment: `fx` is a top-level project collection); design review by reaper/scout still wanted before the command catalogue |
| P4.1 | core-json `compactRows` writer option for bulk arrays in project files | not needed so far: the canonical writer already prints MIDI rows one per line |
| P4.2 | `libs/timeline`: tempo/meter maps, closed-form ramps, bar/beat, pickups | **done** (orchestrator, ba816c6) |
| P4.3 | `host/engine/model`: ids, IdMap, object types + schemas, EditTxn, invariants, diff, project file, VersionStore (undo/redo/coalescing/branches/replay merge with field-level conflicts) | **done** (orchestrator, 4a327ac); review queued (scout-5) |
| P4.4 | `host/engine` offline renderer: plan compile (topological, fixed summation order), MIDI items, audio items (native rate), FX chains via clap-host, mixer (width/volume/pan law/mute, master balance), routes with taps, PDC, mixer automation (track volume/trim/pan/width/mute, route volume/pan/mute; shapes), solo, engine.graph lines; real-time `Engine` on the same plan (RtHandoff plan swap, transport commands over SPSC, instance start/stop on the audio thread, prune after adoption) with a `DummyBackend` (manual / threaded) — offline and real-time output are bit-identical | **done** (orchestrator, 275682b, 52e29cc, b14ce97, fe7ee89). Next: plugin-parameter automation, stems, resampling (needs core-dsp), JACK backend via dlopen, anticipative processing |
| P4.5 | engine command catalogue on the bus (session-model §7) | **core done** (orchestrator, bbb694c, 42b45e2): `EngineApp` (a `cmd::Host`: one version per bus transaction, invocations recorded for replay, plugin catalogue, scratch instances for patch validation, save/open, render) and 32 commands (plus the bus built-ins) — project.get/set/save/open, session.describe, tempo.set, timesig.set, track.add/remove/set/list, item.add/remove/move, midi.write/get, fx.add/remove/set/describe, fx.patch.merge, fx.param.set, render.mix, history.undo/redo/list, branch.create/switch/list/diff/merge/delete (merges replay invocations; field-level conflicts reported). `quire-engine exec`/`registry` CLI and generated `skill/generated/engine-*.{md,json}` with a drift test. Long tail (route.*, env.*, marker/region, take.*, item.set/split/duplicate, fx.move, fx.patch.get/set, fx.param.get, plugin.list/scan, time.convert, transport.*, render.stems) is a Sol task |

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
