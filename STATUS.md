# Status

Updated 2026-09-26 by the orchestrator. Compact and current: what's done,
in progress, blocked, next. Task IDs are stable (P<phase>.<n>).

## Phase 0 — Reconnaissance and foundations

| ID | Task | State |
|---|---|---|
| P0.1 | Template study → `docs/research/template.md` | done (scout) |
| P0.2 | REAPER study → `docs/research/reaper-ux.md` + RPP evidence | done for files/CLI/scripting (reaper); GUI-driven verification (comp lanes, pin maps, isolation menu, render wildcards, action list count via `kbd_enumerateActions`) deferred to before Phase 5 |
| P0.3 | Linux audio survey → `docs/research/linux-audio.md`; ADR-0004 | done |
| P0.4 | GUI/windowing evaluation + spike → ADR-0005 | done (scout spike) |
| P0.5 | Scripting decision → ADR-0006 | done |
| P0.6 | Monorepo, CMake presets, format/lint, local CI, license gate | done |
| P0.7 | Living docs: ARCHITECTURE, ADRs 0001–0011, STATUS, HUMAN_REVIEW, THIRD_PARTY | done |
| P0.8 | Design review of ADRs/specs by a second agent | in progress (scout) |

Acceptance: `tools/ci/build.sh` builds + tests from a clean clone (dev, gcc,
rtsan verified); ADRs for audio backend (0004), GUI (0005), scripting (0006).

## Phase 1 — Shared core libraries

| ID | Task | Spec | State |
|---|---|---|---|
| P1.1 | core-json (value, parser w/ locations, canonical writer, pointer, merge patch, diff, fuzz) | `specs/core-json.md` A | in progress (json, branch p1-json) |
| P1.2 | core-schema (typed binding, validation with hints, units, JSON Schema export) | `specs/core-json.md` B | in progress (json) |
| P1.3 | midi (model, canonical order, SMF I/O) | `specs/notation.md` A | in progress (notation, branch p1-notation) |
| P1.4 | notation (compiler, decompiler, round-trip law, corpus, fuzz) | `specs/notation.md` B–C | in progress (notation) |
| P1.5 | core-params (semantic params, units, curves, normalization, text) | `specs/core-params.md` | spec next |
| P1.6 | core-patch (patch documents, merge, validate, canonical, describe) | `specs/core-patch.md` | spec next |
| P1.7 | core-analysis: loudness (BS.1770/R128, true peak, LRA) validated against Tech 3341/3342 signals | `specs/core-analysis.md` | spec next |
| P1.8 | core-analysis: spectral, harmonic, envelope, stereo, comparison, masking, reports | `specs/core-analysis.md` | after P1.7 |
| P1.9 | core-dsp: oversampling, filters, smoothing, interpolation, denormals | — | after P1.7 |
| P1.10 | audio-file: WAV read/write, FLAC/MP3 decode, resampler | — | with P1.7 |
| P1.11 | command-bus: registry, descriptors, dispatcher, batch, doc/schema export, example-tests | `specs/command-bus.md` | spec after core-json lands |

Acceptance: unit tests per library; loudness validated against EBU R128
reference signals; notation round-trips its corpus; example analysis
reports committed in docs.

## Phase 2 — Sandbox host and the first MCP loop

| ID | Task | State |
|---|---|---|
| P2.1 | `specs/semantic-params-extension.md` (CLAP ext `com.suppleaudio.semantic-params/1`) | planned |
| P2.2 | host/clap-host: load, instantiate, activate, process with note/param events, state, semantic ext | planned |
| P2.3 | test instrument from the template (simple subtractive synth with semantic params) — proves template instantiation + extension | planned |
| P2.4 | host/sandbox: render notation/MIDI/audio through a plugin offline, analyze, write WAV + report; CLI | planned |
| P2.5 | control-protocol: JSON-RPC 2.0 NDJSON over Unix socket | planned |
| P2.6 | mcp adapter (stdio) generated from the registry; sandbox tools | planned |
| P2.7 | scripted end-to-end MCP test + transcript in `docs/demos/` | planned |

## Phase 3 — Stock plugins

| ID | Task | State |
|---|---|---|
| P3.1 | flagship synth: architecture + patch format spec | planned |
| P3.2 | synth DSP modules (VA/wavetable/FM/noise/sub osc; ladder/SVF/comb/formant filters; envs, LFOs, step mods; mod matrix; drive; post-FX; poly/unison/glide/MPE) | planned |
| P3.3 | parameter optimizer (CMA-ES, multi-descriptor loss) as a sandbox tool | planned |
| P3.4 | synth evaluation suite (descriptive + reference-matching tasks) and listening queue | planned |
| P3.5 | EQ | planned |
| P3.6 | dynamics (comp/limiter with sidechain, gate/expander, transient shaper) | planned |
| P3.7 | saturation family with oversampling | planned |
| P3.8 | delay (tempo-synced) and algorithmic reverb | planned |
| P3.9 | utility + live analyzer publishing core-analysis data | planned |
| P3.10 | clap-validator built in user space; per-plugin REAPER load test | planned |
| P3.11 | optional REAPER bridge (ReaScript + socket/MCP adapter) | planned |

## Team (Codex workers via Herdr)

- **scout** (Sol) — research and review. Now: design review (P0.8).
- **reaper** (Sol) — REAPER research; idle until the REAPER GUI pass.
- **json** (Sol) — P1.1/P1.2 on `p1-json`.
- **notation** (Sol) — P1.3/P1.4 on `p1-notation`.

## Blocked / waiting on Nate

See `HUMAN_REVIEW.md` (name veto, license choice; FYIs on location, kernel,
fonts). Nothing is blocked.
