# Status

Updated 2026-09-26 02:30 by the orchestrator. Compact and current: what's done,
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
| P1.3 | midi (model, canonical order, SMF I/O) | `specs/notation.md` A | first cut committed on p1-notation; being revised for the amended SMF API (paused: Codex usage limit until 05:38) |
| P1.4 | notation (compiler, decompiler, round-trip law, corpus, fuzz) | `specs/notation.md` B–C | in progress (notation; paused, uncommitted work in its worktree) |
| P1.5 | core-params (semantic params, units, curves, normalization, text) | `specs/core-params.md` | **done** (orchestrator); review by scout pending |
| P1.6 | core-patch (patch documents, merge, validate, canonical, describe) | `specs/core-patch.md` | **done** (orchestrator); review by scout pending |
| P1.7 | core-analysis: loudness (BS.1770/R128, true peak, LRA) validated against Tech 3341/3342 signals | `specs/core-analysis.md` | spec frozen (exact EBU cases); queued (analysis worker) |
| P1.8 | core-analysis: spectral, harmonic, envelope, stereo, comparison, masking, reports | `specs/core-analysis.md` | after P1.7 |
| P1.9 | core-dsp: oversampling, filters, smoothing, interpolation, denormals | — | after P1.7 |
| P1.10 | audio-file: WAV read/write, FLAC/MP3 decode, resampler | `specs/core-analysis.md` §9 | **done** except the resampler (orchestrator; libs/audio-file). Resampler moved to the analysis brief (core-dsp) |
| P1.11 | command-bus: registry, descriptors, dispatcher, batch, doc/schema export, example-tests, jobs | `specs/command-bus.md`, `specs/optimizer.md` §4 | spec frozen; next for json |

Acceptance: unit tests per library; loudness validated against EBU R128
reference signals; notation round-trips its corpus; example analysis
reports committed in docs.

## Phase 2 — Sandbox host and the first MCP loop

| ID | Task | State |
|---|---|---|
| P2.1 | `specs/semantic-params-extension.md` (CLAP ext `com.suppleaudio.semantic-params/1`) | spec frozen; ABI header in `plugins/common/include/supple/clap/semantic_params.h` |
| P2.2 | host/clap-host: load, instantiate, activate, process with note/param events, state, semantic ext | **done** (orchestrator; fixtures + checked against the template's Starter.clap); review pending |
| P2.3 | probe synth from the template (instrument, CLAP-only offline build, C++20) | brief ready (probe worker, after 05:38; stage 2 unblocked). Done already by the orchestrator: `plugins/common` (`plugin_support.h`: ParamStore, ClapParams, FlatSemantic; `semantic-conformance` tool, 31/31 on a helper-built fixture), `plugins/CMakeLists.txt`; clap-wrapper/Pugl vendored with template patches pre-applied |
| P2.4 | host/sandbox: render notation/MIDI/audio through a plugin offline, analyze, write WAV + report; CLI | **in progress (orchestrator), uncommitted**: `host/sandbox` offline renderer (`render.h/.cpp`: chain on a dedicated audio thread, RT-annotated block loop, events at absolute samples per target, latency compensation, silent-tail detection, transport) + `tests/test_render.cpp`. 5/6 tests pass; the tail test fails because the silent-hold counter counts whole blocks — fix: count only samples past the musical end (`beyond = pos - max(blockStart, musicalEnd)` in `renderLoop`). Next: commit; then Session (instances/chain/renders/settings), notation→events (after notation merges), sandbox commands on the bus (after command-bus), CLI |
| P2.5 | control-protocol: JSON-RPC 2.0 NDJSON over Unix socket | planned |
| P2.6 | mcp adapter (stdio) generated from the registry; sandbox tools | planned |
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
| P4.0 | `specs/session-model.md` (objects, time map, versions/branches, project file, command catalogue) and `specs/engine.md` (plans, PDC, anticipative processing, isolation, benchmarks) | drafts; to be reviewed before P4 |
| P4.1 | core-json `compactRows` writer option for bulk arrays in project files | planned |

## Team (Codex workers via Herdr)

Codex usage limit reached 01:50; resets 05:38 (a one-shot session cron fires
05:39 to resume). Roster + briefs: scratchpad `roster.md`, `briefs/*.md`
(json-2: command bus then RPC/MCP; analysis-1; probe-1; scout-4: review of
orchestrator code + Phase 3/4 draft specs). Until the reset the orchestrator
works alone.

- **scout** (Sol) — research and review; idle.
- **reaper** (Sol) — REAPER research; idle.
- **json** (Sol) — P1.1/P1.2 done; next P1.11 command-bus.
- **notation** (Sol) — P1.3/P1.4, paused mid-task.
- Queued: **analysis** (P1.7–P1.9 + resampler), **probe** (P2.3).
- Orchestrator-written, review by scout pending (brief scout-4): core-base RT
  primitives (SpscRing, RtHandoff, Rng), core-params, core-patch, clap-host,
  plugins/common, audio-file, sandbox renderer.
- On resume: notation must rebase onto main (tests/fuzz scaffolding from
  p1-json now on main; SMF API amendments).

## Blocked / waiting on Nate

See `HUMAN_REVIEW.md` (name veto, license choice; FYIs on location, kernel,
fonts). Nothing is blocked.
