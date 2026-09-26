# Status

Updated 2026-09-26 06:25 by the orchestrator. Compact and current: what's done,
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
| P2.3 | probe synth from the template (instrument, CLAP-only offline build, C++20) | stage 1 **done** (probe; commit 9494f9a on p2-probe, not merged: 8-voice instrument, CLI render, editor, X11 embed check passed). Review pending. Stage 2 (semantic params via plugin_support — now with `ParamStore::readAll()`) on hold until the reset |
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
| P4.0 | `specs/session-model.md` (objects, time map, versions/branches, project file, command catalogue) and `specs/engine.md` (plans, PDC, anticipative processing, isolation, benchmarks) | drafts; to be reviewed before P4 |
| P4.1 | core-json `compactRows` writer option for bulk arrays in project files | planned |

## Team (Codex workers via Herdr)

Roster + briefs: `~/Projects/quire/.orchestrator/` (git-excluded; the old
session scratchpad is retired). Codex 5h window used up 05:45→06:05 by five
workers; resets 10:45. Weekly budget is the binding constraint (84% at 05:45,
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
