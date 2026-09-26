# Status

Updated 2026-09-26 07:05 by the orchestrator. Compact and current: what's done,
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
| P1.4 | notation (compiler, decompiler, round-trip law, corpus, fuzz) | `specs/notation.md` B–C | **done** (notation; merged 999c0b2: 45 valid + 32 invalid corpus docs, 2000-case round trip, fuzz). Review by scout pending. Queued amendment: bars before any `voice` line = implicit `voice main`, and a correct hint for that case (today: "did you mean 'ppq'?") |
| P1.5 | core-params (semantic params, units, curves, normalization, text) | `specs/core-params.md` | **done** (orchestrator); reviewed (scout round 2), fixes in 6d62923 |
| P1.6 | core-patch (patch documents, merge, validate, canonical, describe) | `specs/core-patch.md` | **done** (orchestrator); reviewed (scout round 2), fixes in 15b7324 |
| P1.7 | core-analysis: loudness (BS.1770/R128, true peak, LRA) validated against Tech 3341/3342 signals | `specs/core-analysis.md` | in progress (analysis, branch p1-analysis; uncommitted core-dsp + resampler + loudness work; paused at the Codex limit 06:05) |
| P1.8 | core-analysis: spectral, harmonic, envelope, stereo, comparison, masking, reports | `specs/core-analysis.md` | after P1.7 |
| P1.9 | core-dsp: oversampling, filters, smoothing, interpolation, denormals | — | after P1.7 |
| P1.10 | audio-file: WAV read/write, FLAC/MP3 decode, resampler | `specs/core-analysis.md` §9 | **done** except the resampler (orchestrator; reviewed, fixes: truncated-WAV flag, NaN PCM, atomic writes). Resampler in progress on p1-analysis |
| P1.11 | command-bus: registry, descriptors, dispatcher, batch, doc/schema export, example-tests, jobs | `specs/command-bus.md`, `specs/optimizer.md` §4 | **done** (json; merged bd33e44; reviewed by the orchestrator: dispatch/batch/transactions/error paths sound). Follow-up: the meta commands' summaries are terse ("Run commands atomically.") — batch is an MCP tool, give it a real description |

Acceptance: unit tests per library; loudness validated against EBU R128
reference signals; notation round-trips its corpus; example analysis
reports committed in docs.

## Phase 2 — Sandbox host and the first MCP loop

| ID | Task | State |
|---|---|---|
| P2.1 | `specs/semantic-params-extension.md` (CLAP ext `com.suppleaudio.semantic-params/1`) | spec frozen; ABI header in `plugins/common/include/supple/clap/semantic_params.h` |
| P2.2 | host/clap-host: load, instantiate, activate, process with note/param events, state, semantic ext | **done** (orchestrator); reviewed (scout round 2); fixes 15b7324: enforced lifecycle, pending restart/flush requests, event-arena overflow, tail notifications; new `test.edge` fixture |
| P2.3 | probe synth from the template (instrument, CLAP-only offline build, C++20) | stage 1 **done and merged** (11e3892; `build/<preset>/plugins/clap/ProbeSynth.clap`, its 6 ctests run as `plugin.probe-synth.ctest`; `supple_add_plugin(<dir> <PREFIX>)` made generic). Review pending. Stage 2 (semantic params via plugin_support — now with `ParamStore::readAll()`) at the reset |
| P2.4 | host/sandbox: render notation/MIDI/audio through a plugin offline, analyze, write WAV + report; CLI | **done except analysis** (orchestrator): renderer (147336b + review fixes), `Session` (107f7f4: instances/chain/params/patches/presets/renders/transactions), 16 commands on the bus + `SandboxHost` with NDJSON command log, `quire-sandbox exec`/`registry` CLI, generated `skill/generated/sandbox-*.{md,json}` with a drift test. Examples use the test fixtures until the probe synth is semantic. Missing: `analyze`/`compare`/`masking`/`profile.evaluate` + render `analyze` levels (need core-analysis), audio-file resampling, `--mcp` (needs P2.6) |
| P2.5 | control-protocol: JSON-RPC 2.0 NDJSON over Unix socket | next for json (brief json-2 Task B) |
| P2.6 | mcp adapter (stdio) generated from the registry; sandbox tools | next for json (Task B), then `--mcp` in the sandbox CLI (orchestrator) |
| P2.7 | scripted end-to-end MCP test + transcript in `docs/demos/` | planned |

## Phase 3 — Stock plugins

| ID | Task | State |
|---|---|---|
| P3.1 | flagship synth "Orrery": architecture + patch format spec | draft `specs/synth.md`; Nate's review queued (HUMAN_REVIEW #6, #7) |
| P3.2 | synth DSP modules (VA/wavetable/FM/noise/sub osc; ladder/SVF/comb/formant filters; envs, LFOs, step mods; mod matrix; drive; post-FX; poly/unison/glide/MPE) | planned |
| P3.3 | parameter optimizer (CMA-ES, multi-descriptor loss) as a sandbox tool | draft `specs/optimizer.md` |
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

Roster + briefs: `~/Projects/quire/.orchestrator/` (git-excluded; the old
session scratchpad is retired). Codex 5h window used up 05:45→06:05 by five
workers; resets 10:45 (weekly 69% left at 07:20). Weekly budget is the binding constraint (84% at 05:45,
~16–18% per full window), so Codex goes to critical-path work and reviews.

- **json** (Sol) — P1.11 done and merged. Next: Task B (control protocol + MCP).
- **notation** (Sol) — P1.3/P1.4 done and merged. Next: the implicit-voice amendment.
- **analysis** (Sol) — P1.7–P1.9 + resampler in progress (uncommitted in its worktree; resume with "continue", then commit via escalation).
- **probe** (Sol) — stage 1 done (on p2-probe). Next: rebase onto main, stage 2.
- **scout** (Sol) — code review round 2 done (all findings fixed or noted). Next: review notation, command-bus, sandbox Session/commands, probe stage 1.
- **reaper** (Sol) — idle; later: design review of Phase 3/4 draft specs.
- Codex workers cannot write the worktrees' shared `.git` without sandbox
  escalation; they are told they may escalate for git in their own worktree.

Not yet fixed from review round 2 (minor, recorded): describeFlat skips hidden
params in Full detail; core-params formatting of extreme magnitudes (>6
decimals); Rng::uniform extreme endpoints; RtHandoff forced-schedule tests.

## Blocked / waiting on Nate

See `HUMAN_REVIEW.md` (name veto, license choice; FYIs on location, kernel,
fonts). Nothing is blocked.
