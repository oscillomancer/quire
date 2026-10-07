# Status

Updated 2026-10-06 by the orchestrator. Compact and current: what's done,
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

| P1.12 | console9: the Airwindows Console 9 Channel/Buss port, null-tested against the reference (build prompt §3, added 2026-09-30) | `research/console9.md`, ADR-0019 §7–§9 | **done** (Codex `margin`, reviewed by the orchestrator): `libs/console9` Channel/Buss stages, bit-identical against the vendored reference (test-only `third_party/airwindows-console9`), seeded dither restartable at locate, level-matched linear mode for "Console 9 off" (ADR-0019 §6) |

Acceptance: unit tests per library; loudness validated against EBU R128
reference signals; notation round-trips its corpus; example analysis
reports committed in docs. The Console 9 port null-tests against the reference.

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
| P3.2a | Orrery CLAP core + editor bridge | **done** (dc0366a + this session): `plugins/orrery/clap` (params, state, semantic extension over the structured patch, `orrery-headless.clap`), `EditorBridge` (gesture-bracketed edits to host + engine, structural merges, telemetry, extension hook for the template plugin's GUI), `orrery/edits.h` (structural edits as merge patches: add/remove/retype with route cascades, fx chain, routes, available sources/destinations — every offered route is tested against the codec). **Plugin with editor done** (`plugins/orrery`, template-derived; `Orrery.clap`): signal-flow diagram, modulators, inspector, routes, menus; stereo/mono and 64-bit; tests: editor-input, host-smoke (stereo + mono), semantic conformance, wrapper checks — in every CI preset. Wavetables done (42a98fe). **Loaded and exercised in REAPER 7.80 on this machine** (2026-09-26, release build in ~/.clap): 223 params, MIDI phrase → render at −8.1 dBFS peak, editor embedded (current acceptance: `python3 tools/reaper-check/check.py --editor --plugin com.suppleaudio.orrery`). Next: clap-validator (P3.10, probe-3), performance |
| P3.2 | synth DSP modules (VA/wavetable/FM/noise/sub osc; ladder/SVF/comb/formant filters; envs, LFOs, step mods; mod matrix; drive; post-FX; poly/unison/glide/MPE) | **core done** (orchestrator, 3663fe4): `plugins/orrery/engine` — 230-param table, patch codec (whole-patch validation, merge, describe, JSON Schema), real-time engine (bit-identical across block sizes, seeded, wait-free program swap), all module types except real wavetables (sine placeholder), FX rack, IIR half-band decimation; 37 tests. CLAP core + headless module in progress. Performance (Codex `margin`, reviewed by the orchestrator; `docs/benchmarks/orrery-perf-2026-09-30.md`): reference patch 13.8 % → 9.1 % of one core (process CPU, 48 kHz/256, pinned) — float ladder lanes across voices, specialized oscillator/drive kernels, cached conversions; renders within −123 dB of the base for every recipe, still bit-identical across block sizes. The 3 % target needs a structural pass (structure-of-arrays voice bank, oscillator lanes across voices, fused routing/filter/amp path) — queued. Other open items: synth.md implementation notes |
| P3.3 | parameter optimizer (CMA-ES, multi-descriptor loss) as a sandbox tool | **done** (orchestrator, aa53962): `libs/optimize` CMA-ES; sandbox `optimize.start` job (reference and profile targets, trajectory term, "auto" parameter selection); end-to-end test recovers a hidden Orrery patch within 10 %. Review round 5 fixes (clone lifecycle on one thread, reference analysis off the model thread, prompt cancel, profile validation/weights, envelope terms, render metadata, thread reaping) → analysis (brief analysis-3) |
| P3.4 | synth evaluation suite (descriptive + reference-matching tasks) and listening queue | draft `specs/eval-harness.md`; model under test: Hermes agents with their default model (HUMAN_REVIEW #8, resolved); **harness done** (Codex `contour`, reviewed by the orchestrator; `tools/eval-harness/`, spec finalized): 25 tasks (10 descriptive, 10 tiered reference, 5 notation), dry and Hermes runners (isolated Hermes home per attempt, sandbox MCP only, final patch captured after every tool call), scorer, reports, listening queue; `--jobs`, `--blind`, Hermes preflight added by the orchestrator. **First campaign done** (`docs/benchmarks/eval/2026-10-01-campaign.md`, muse-spark-1.3-contributor, 3 repeats, ≈ $3.5): notation 14/15, descriptive 30/30 with targets shown (a floor), **blind descriptive 25/30**, reference 9/30 (confounded). Findings queued below (P3.4a–e). **Every campaign of 2026-09-30..10-01 ran a stale Orrery** installed in `~/.clap` (P3.4h): the reference numbers (9/30, 8/30) are void; no rerun: Nate (2026-10-01) judged the model competent with the tools by listening to the renders and found the scores not very useful (HUMAN_REVIEW #15/16) |
| P3.4a | renders in a reused instance depended on the previous render (eval reference acid bass: decay20 221 vs 511 ms) | **fixed** (orchestrator, 4a9e8f5): parameter ramps, the FX delay time, the output-gain ramp and tick bookkeeping survived `reset()`; found by replaying the agent's call log; regression test in the engine suite |
| P3.4b | reference loss: envelope time terms unbounded (5877) | **done** (Hermes `scoring`, reviewed by the orchestrator, f607f02): log-time distance `|log2((c+10)/(r+10))|`, scale 0.43; one-sided ≈ 11.1 unchanged |
| P3.4c | optimizer: 9-parameter auto-range searches never improve a one-parameter-off patch (sub bass, 291 evaluations) | **done** (Hermes `scoring`, reviewed by the orchestrator): `tools/optimize-bench` (300 evaluations × 3 seeds over the reference tasks, two modes); kept σ = 0.05 for reference targets and "auto" ranges as a window around each current value (±2 octaves for Hz/ms/st/ct/oct, ±25 % of the range otherwise): auto 0/21 → 3/21 passes (median loss 21.8 → 3.3), changed-modules 9/21 → 10/21 (3.0 → 1.1); restart and fresh-state variants measured and dropped; failed candidates are counted (`docs/benchmarks/optimizer/2026-10-01-tuning.md`). Found: `plugin.describe` reports stale `active` flags after a patch adds a module (P3.4g) |
| P3.4g | `plugin.describe` uses the schema captured at load: `active` flags go stale after a patch adds a module | **done** (Hermes `scoring`, reviewed by the orchestrator) |
| P3.4h | "state-dependent renders" in evals: in-session renders 348 vs the scorer's 585 ms (sub bass) | **fixed** (orchestrator, 28134d1): not render state. The catalogue sorted an id's copies by *path name* and scan() sorted all files globally, so `~/.clap/Orrery.clap` (a release installed 2026-09-30, before the reset fix) shadowed the build's for every session loading by id. Now the first location on the search path wins; `plugin.list`/`plugin.scan` report `path` and `shadowed`. Harness (Hermes `scoring` + orchestrator, 4191d21): every session's plugin files recorded and checked against the build (a mismatch stops the run), and a run whose attempts all broke off is never published |
| P3.4d | `compare` reports the scored loss; `job.wait` timeout | **done** (Hermes `scoring`, f607f02/8c3ae3d): `compare` returns `loss {total, terms, note_off_s}` from the same function as the scorer (equal to 9 places); `job.wait` clamps to 30 s and says so |
| P3.4e | notation accepts `| N:` when N is the next bar | **done** (Hermes `notation`, 7091475) |
| P3.4f | every plugin: a render after `reset()` depends only on its state (the P3.4a class of bug) — generic `plugin.<id>.reset` test + fixes | **done** (Hermes `notation`, reviewed by the orchestrator): `clap-reset-check` drives each plugin like the sandbox (S1 render, reset, S2 vs a fresh S2, bit-identical at blocks 256 and 64, with non-vacuity guards) for all 11 plugins + headless Orrery; under RTSan it found that Orrery's `params.flush` allocated on the audio thread while active-but-stopped (compiled a program) — fixed with an allocation-free `Engine::setValues` |
| P3.5 | EQ | **done** (Facet [`eq`], Codex `contour`, reviewed by the orchestrator; `docs/specs/effects.md` §2): 8 bands (bell, shelves, tilt, notch, band-pass, Butterworth HP/LP 6–96 dB/oct), stereo/L/R/M/S per band, three phase modes — `minimum` (prewarped SVFs), `natural` (default; matched second-order sections after Vicanek, replacing a 16×–128× oversampled first version that cost 50× the CPU: now 0.24 %/0.43 % of a core for 4/8 bands at 48 kHz, bells/normal shelves within 0.3 dB of the analog prototype to 20 kHz, measured limits for resonant and near-Nyquist bands) and `linear` (partitioned FIR, 2048 samples at 48 kHz); auto gain; response + live spectrum GUI. REAPER check passed (2026-09-30) |
| P3.6 | dynamics (comp/limiter with sidechain, gate/expander, transient shaper) | **compressor/limiter done** (Ballast [`comp`], Codex `press`, reviewed by the orchestrator — the limiter's attack follows the lookahead moving average and true peak is detected on the 4× interpolated signal, both fixed after review; effects.md §3): soft-knee feed-forward/feedback compressor with external sidechain (ADR-0018 port), key filters and listen, peak/RMS, auto release/makeup, lookahead, mix; limiter mode with true peak (+12 dB pink noise → −1.00 dBFS / −0.99 dBTP under a −1 dBTP ceiling). **Gate/expander done** (Sluice [`gate`], Codex `margin`, reviewed by the orchestrator; effects.md §8): state-machine gate with hysteresis and hold, downward expander, filtered key (external sidechain, listen), stereo link, lookahead with the opening ramp aligned to the delayed onset, range −80 = exact silence; a mono key bus feeds both detectors (fixed in the gate after review, and in the compressor). **Transient shaper done** (Flint [`transient`], Codex `press`, reviewed by the orchestrator; effects.md §9): level-independent dual-envelope (dB-domain) detector with a cycle-aware peak hold, attack/sustain ±12 dB, optional 2× oversampled tanh clip on the shared `dsp::Oversampler` (72 samples of latency, only while clip is on). REAPER check passed (2026-09-30) |
| P3.7 | saturation family with oversampling | **done** (Kiln [`drive`], Codex `trim`, reviewed by the orchestrator; effects.md §7): six odd-symmetric curves (tape, tube, transistor, diode, fold, hard) with first-order ADAA, bias (even harmonics) with a 5 Hz DC blocker, pre/post tone tilt, Gaussian-RMS auto gain (within 0.36 LU at −18 dBFS RMS pink noise, fold 0.91), and the shared `dsp::Oversampler` in `libs/core-dsp` (linear-phase half-bands 145/33/25 taps; latency 1/73/81/84 samples at 1x/2x/4x/8x — the first version's 1025-tap stage cost 12 ms and was redesigned after review; aliases in 20 Hz–20 kHz ≤ −104 dBc at 4x for the smooth curves). REAPER check passed (2026-09-30) |
| P3.8 | delay (tempo-synced) and algorithmic reverb | **delay done** (Echelon [`delay`], Codex `margin`, reviewed by the orchestrator; effects.md §10): stereo/ping-pong/mono lines to 5 s, CLAP-transport tempo sync with note values, tape-like two-pole time glide, wow, loop filters, drive and a safety limiter (feedback to 110 % self-oscillates below 0 dBFS), wet ducking, width, freeze, CLAP tail extension. REAPER check passed (2026-09-30). **Reverb done** (Nave [`reverb`], Codex `contour`, reviewed by the orchestrator; effects.md §11, stock-effects.md §7): seeded early reflections, 4-stage input diffusion (capped below half the shortest band tail), 16-line Hadamard FDN with twelve-point Lagrange reads, three-band RT60 (Butterworth-Q shelves; worst RT60 error 7.7 % over the size/decay/crossover/rate matrix), seeded modulation, freeze, width, CLAP tail; 1.5–1.8 % of one core at 48 kHz stereo. Test matrix trimmed to a CI subset plus an opt-in measurement runner |
| P3.9 | utility + live analyzer publishing core-analysis data | **utility done** (Plumb [`utility`], Codex `trim`, reviewed by the orchestrator; `docs/specs/effects.md` §4): gain/balance/width/channel modes/swap/polarity, bass mono as an LR4 high-pass on the side only (zero latency, exact mono sum), DC block, mute; bit-exact at defaults, block-size invariant, readonly level/correlation meters, compact editor. **Analyzer done** (Gauge [`analyzer`], Codex `margin`, reviewed by the orchestrator; effects.md §5): bit-exact pass-through, the audio thread only copies into an SPSC packet ring, a consumer thread measures with the new `core-analysis` `LiveMeter` (rolling-window `LoudnessMeter`: M/S/I, LRA, true peak; correlation, width, balance, 1/3-octave spectrum, centroid), readonly CLAP meters, and the appended `get_telemetry` member of the semantic-params extension (analysis-report JSON; old-size extensions still accepted). REAPER check passed (2026-09-30). Open: the engine does not read plug-in telemetry yet |
| P3.10a | one display-name definition per stock plugin (HUMAN_REVIEW #9 renames become one line) | **done** (Codex `press`, reviewed by the orchestrator): `plugins/<id>/cmake/Identity.cmake` is the only literal; artifacts are the display names (Sluice.clap, Gauge.clap now too) |
| P3.10b | Linux export lists + VST3 self-module lifetime (Nate's template fix 3d32b8d, HUMAN_REVIEW #12) | **done** (orchestrator, 7bde7cb): every plugin exports only `clap_entry` (VST3 also the module entry points); the vendored clap-wrapper no longer retains its own handle; per-plugin `plugin.<id>.unload` test loads, unloads and reloads the CLAP |
| P3.10 | clap-validator built in user space; per-plugin REAPER load test | **done** (clap-validator: every stock plugin in CI; REAPER: `tools/reaper-check` (Codex `trim`, reviewed by the orchestrator) renders each plugin headless in REAPER with a private config (the user's `~/.config/REAPER` checksummed before/after, dummy audio, process groups killed on timeout): load, signal vs bypass, one automated parameter, state; `--editor` opens each editor. REAPER 7.81, 2026-09-30, merged build d2584d0: all ten (analyzer, comp, delay, eq, transient, gate, drive, orrery, utility, probe-synth) pass load, signal, automation, state and editor) |
| P3.11 | optional REAPER bridge (ReaScript + socket/MCP adapter) | planned |

## Phase 4 — Headless engine (specs drafted early)

**Phase 4 accepted 2026-10-01**: every criterion below passes or is recorded (B1/B2 on the real device, Console 9 invariants).

Acceptance (build prompt Phase 4):

| Criterion | State |
|---|---|
| Multi-track session with CLAP plugins in real time at 64-sample buffers without xruns under a defined load | **passes with anticipation** (B1, `docs/benchmarks/b1.md`, 2026-10-01): 30 tracks of Orrery reference patch → Facet → Ballast + a monitored input chain + master limiter at 48 kHz / 64 frames, 0 xruns over a confirmed 300 s (MOTU M4, PipeWire-JACK). Without anticipation N = 1 already xruns — per-block bursts; diagnostic queued (P4.21) |
| Offline render bit-identical across repeated renders; nulls against real-time capture | **passes** (engine.tests: RT ≡ offline in both modes, B4 determinism for any thread count, with loops, monitoring, automation) |
| Undo/redo fuzz-tested | **passes** (p4-fuzz: history property fuzzer + seeded engine test; project-file fuzzer) |
| A crashing plugin in isolation does not take down the engine | **passes** (P4.8; shared bridge in review) |
| An LLM over MCP builds a small arrangement headlessly and renders it | **passes** (P4.14, engine.mcp-arrangement) |
| Benchmarks vs REAPER recorded in docs/benchmarks/ | **recorded** (`b1.md`, `b2.md`): Quire 30 tracks (anticipative) vs REAPER 7.81 default anticipative FX failing at N = 1 on the same session; REAPER's count is a timestamp-change count (≥ 1 underrun) |
| Console 9 invariants hold (strip-layout fuzz test; fader/pan drive C9 Channel or the master's plain stage) — added 2026-09-30 | **passes** (Codex `press`, reviewed by the orchestrator; ADR-0019, `docs/benchmarks/console9-strips.md`): typed channel/buss/master strips (project format 2.0 with migration), implicit Channel/Buss stages, source-owned send encoders, pre-encode mute/solo ramps, level-matched linear mode with a crossfaded switch, seeded dither restarted at locate, decoded stems; plan checker + strip fuzz tests; unity channel → master −6.52 dB, silence peak −152 dBFS; 38/70 ns per frame for a channel/buss strip (release) |

| ID | Task | State |
|---|---|---|
| P4.0 | `specs/session-model.md` (objects, time map, versions/branches, project file, command catalogue) and `specs/engine.md` (plans, PDC, anticipative processing, isolation, benchmarks) | drafts, used as the basis for the P4 core below (amendment: `fx` is a top-level project collection); design review by reaper/scout still wanted before the command catalogue |
| P4.1 | core-json `compactRows` writer option for bulk arrays in project files | not needed so far: the canonical writer already prints MIDI rows one per line |
| P4.2 | `libs/timeline`: tempo/meter maps, closed-form ramps, bar/beat, pickups | **done** (orchestrator, ba816c6) |
| P4.3 | `host/engine/model`: ids, IdMap, object types + schemas, EditTxn, invariants, diff, project file, VersionStore (undo/redo/coalescing/branches/replay merge with field-level conflicts) | **done** (orchestrator, 4a327ac); review queued (scout-5) |
| P4.4 | `host/engine` offline renderer: plan compile (topological, fixed summation order), MIDI items, audio items (resampled to the plan's rate on load, cached per rate; the real-time engine runs at the device's rate whatever the project's), FX chains via clap-host, mixer (width/volume/pan law/mute, master balance), routes with taps, PDC, mixer automation (track volume/trim/pan/width/mute, route volume/pan/mute; shapes), solo, engine.graph lines; real-time `Engine` on the same plan (RtHandoff plan swap, transport commands over SPSC, instance start/stop on the audio thread, prune after adoption) with a `DummyBackend` (manual / threaded) — offline and real-time output are bit-identical | **done** (orchestrator, 275682b, 52e29cc, b14ce97, fe7ee89). Next: stems; a disk-backed, background media cache (resampling is synchronous at plan compile today) |
| P4.6 | audio backends (ADR-0004): JACK via dlopen | **done** (orchestrator, this session): `JackBackend` (own symbol table, loaded once per path; block-sized callbacks whatever the period; `PIPEWIRE_LATENCY` request, `forceBufferSize` opt-in; xrun/rate-change/shutdown reporting; final zero-frame callback on the JACK thread, 2 s fallback), tested against a stand-in libjack (period/rate changes, xruns, stalls, server shutdown, engine playback, bus playback). Real device (opt-in test): 48 kHz, 128-frame period granted for a 64-frame request, 0 xruns. `engine.start/stop/status`, `transport.play/pause/stop/seek/get`, `quire-engine play FILE`. Open: automatic restart on device change, JACK MIDI ports, deadline-miss xruns, the forced 64-frame period for B1 |
| P4.10 | recording (ADR-0014 phase 1) | **done** (orchestrator, this session): armed tracks' inputs captured on the audio thread into SPSC rings, written by a writer thread to float WAV with periodic header rewrites (crash-readable), placed by play position minus plan + device latency; `track.set {armed, record_input, monitor}`, `transport.record`, takes committed as one version on stop; crash recovery — a sidecar per recording, header repair, `project.recoverable`/`project.recover`, `recoverable` count on `project.get/open`. Open: punch/loop takes, RF64 |
| P4.11 | input monitoring — the live region (ADR-0014 phase 2) | **done** (orchestrator, this session): live classification (monitoring + downstream + master), monitored input in the node's input sum (also while stopped), two executors over one plan — the renderer renders ahead and fills per-frontier-edge rings, the callback runs live nodes at the device block and switches plans at the render horizon; plans reclaimed by generation; per-instance owner tokens. Anticipative output with a monitored track is bit-identical to the in-callback reference; tsan/rtsan clean. A node leaving the live region while playing stays live, on the same instance, until the transport stops (2026-10-04; the fresh renderer instance it used to get lost plugin state and crashed open editors); the idle renderer follows the callback while all is live. Open: live multiprocessing |
| P4.12 | transport loop, punch, loop recording into takes (ADR-0015) | **done** (orchestrator, this session): loop playback in both modes and with monitoring (wraps mid-block without a gap; stream positions for cross-executor comparisons; held notes released at the wrap; tails ring on), punch applied at commit, one item per loop with a take per pass (newest whole pass active), `transport.loop/punch`, CLAP transport loop flags; epoch, seek and loop published under one sequence lock (the audio thread never waits for a writer; review 13). Mix and bypass changes ramp over 10 ms across plan swaps. Open: loop passes spanning a loop change |
| P4.13 | MIDI input: live monitoring and recording (ADR-0016) | **done** (orchestrator, this session): JACK MIDI input ports per device (optional symbols), Dummy MIDI for tests; MIDI-monitored tracks play live in the callback (device/channel routes); MIDI takes committed as notes/controls per run or loop pass, punch-trimmed; `track.set {record_midi, midi_channel, record_mode}` (midi_overdub merges as a new take). Open: MIDI crash recovery, MIDI outputs |
| P4.14 | Phase 4 acceptance: an arrangement over MCP | **passes** (orchestrator, this session): `tests/integration/mcp_arrangement.py` (CTest `engine.mcp-arrangement`, dev/gcc) — an MCP client builds tempo + three Orrery tracks with patches and notation MIDI + a basic mix in one batch, renders, analyzes, saves, reopens and renders bit-identically; transcript in docs/demos/mcp-arrangement.md |
| P4.9 | plugin-parameter automation | **done** (orchestrator, this session): envelopes on plugin params (semantic keys in physical units, interpolated on the parameter's curve; CLAP ids/names in CLAP values), sample-accurate events every 32 samples and at points, latency-aligned, RT ≡ offline; `env.add/points.set/set/remove/list`. Open: write/touch/latch recording, automation items |
| P4.8 | plugin process isolation (ADR-0013) | **done, dedicated mode** (orchestrator, this session): `<slug>-bridge` + bridged `host::Instance` (control socketpair, memfd data plane, futex, 2 s deadline (lifecycle commands 7 s), pidfd); crash/hang fail only the instance; `fx.set {isolation}`, `fx.restart`, `failed_fx`, `fx.failed`. **Phase 4 acceptance "a crashing test plugin in isolation mode doesn't take down the engine" passes** (engine.tests). Shared mode (probe, p4-shared-bridge): shared fx share one bridge process, kept ready while idle. Open: GUI across processes, B6 benchmark |
| P4.7 | anticipative processing + worker pool (ADR-0012) | **done, first cut** (orchestrator, this session): renderer thread renders the whole graph 200 ms ahead in 1024-frame blocks into an epoch-tagged chunk ring (seek/pause flush by epoch, edits at the horizon without gaps, misses counted); DAG worker pool over plan nodes (bit-identical for any thread count; offline renders use it — B4 test). tsan/rtsan clean. Open: live nodes and live multiprocessing (needs monitoring/recording), B1 capacity benchmark (needs the stock EQ/compressor), futex parking tuning |
| P4.5 | engine command catalogue on the bus (session-model §7) | **core done** (orchestrator, bbb694c, 42b45e2): `EngineApp` (a `cmd::Host`: one version per bus transaction, invocations recorded for replay, plugin catalogue, scratch instances for patch validation, save/open, render) and 32 commands (plus the bus built-ins) — project.get/set/save/open, session.describe, tempo.set, timesig.set, track.add/remove/set/list, item.add/remove/move, midi.write/get, fx.add/remove/set/describe, fx.patch.merge, fx.param.set, render.mix, history.undo/redo/list, branch.create/switch/list/diff/merge/delete (merges replay invocations; field-level conflicts reported). `quire-engine exec`/`registry` CLI and generated `skill/generated/engine-*.{md,json}` with a drift test. Long tail (analyze/compare/masking in the engine, route.*, env.*, marker/region, take.*, item.set/split/duplicate, fx.move, fx.patch.get/set, fx.param.get, plugin.list/scan, time.convert, render.stems) is a Sol task; engine.* and transport.* done with P4.6; **long tail** (json-10, finished by the orchestrator): route.add/set/remove/list (cycle-checked through folder sends), item.set/split/duplicate, take.add/activate/set/list, fx.move, fx.patch.get/set, fx.param.get, marker.* / region.*, plugin.list/scan (with format), time.convert; fx.describe {full} lists a non-semantic plug-in's plain parameters. Open: hardware route endpoints, take-FX chain commands |
| P4.15 | render system (engine.md §3, session-model §7) | **done** (json-8, finished by the orchestrator): render.stems (isolated or through the master; tap), render.regions and the project render matrix, render.dry-run, render.mix/stems/regions of another branch's head, branch.render-ab (two renders + analyze.compare), file-name patterns (§7.3, closed token set, filters, empty-token separator dropping, no escape from the renders directory), two-pass normalization (LUFS I/S/M, peak, true peak, RMS; optional true-peak limiter), WAV PCM 16/24 with seeded dither or float, RF64 beyond 4 GiB, render jobs (job: true; progress, cancel), per-file analysis summaries. Open: FLAC (no permissive encoder vendored) |
| P4.16 | VST3 hosting (ADR-0017) | **done** (probe-5, finished by the orchestrator): SDK hosting subset v3.8.0 (MIT) vendored, a VST3 backing inside host::Instance (bounded preallocated parameter/event queues, IMidiMapping, ProcessContext with the loop, state blob, latency), .vst3 scanning, isolation through the bridge. Checked against thirteen installed VST3 classes (sidechain buses, sfizz): all scan, list parameters and render. Open: persisted scan cache / out-of-process scanning (editors: done in Phase 5, in process and bridged) |
| P4.17 | sidechain inputs (ADR-0018) | **done** (orchestrator, this session): a route can end at a plug-in's key (`route.add {from, fx}`), summed per stage and delay-compensated against the stage's input; CLAP process calls carry every declared port (key, silence or scratch), VST3's first auxiliary input bus is the key (activated only while keyed; a change restarts the instance), the bridge carries the key (protocol 4); solo fades what a node sends except keys; RT ≡ offline in both executors, live monitoring included. Open: take-FX keys, multichannel tracks and pin maps |
| P4.18 | Lua scripting on the command bus (ADR-0006) | **done** (lua-1, Hermes; reviewed by the orchestrator): vendored PUC Lua 5.4.9 (MIT) built as C++ (errors unwind as exceptions; TSan covers the scripts), `libs/lua-bus` — sandbox without io/os/debug/package, bindings generated from the registry (`q.track.add{…}`, `q.run`), `q.batch` as one undo step, `q.on/q.off` subscriptions (delivered after the emitting command, and by the service tick), instruction budget and memory cap; `script.run` in the engine and the sandbox; generated LuaLS stubs (`skill/generated/*.lua`, drift-tested; LuaLS itself not run here); `skill/guides/lua.md`. Open: script library path and hot reload (Phase 7); scripts run on the model thread |
| P4.19 | take playrate, pitch and time stretching (session-model §1.5) | **done** (stretch-1, Hermes; reviewed by the orchestrator, who fixed the variant cache key and made the pitch-preserving mode the default): Signalsmith Stretch 1.4 (MIT, seeded, deterministic) and varispeed resampling as media-cache variants; `take.set {playrate, preserve_pitch, pitch, pitch_mode}`; RT ≡ offline. Open: `stretch_markers`; the stretch runs at plan compile (moves to the background decode with the disk cache) |
| P4.20 | the model-thread service tick (engine.md §6) | **done** (json-9, Hermes; reviewed by the orchestrator: buffered MCP stdin, timer teardown, autosave back-off): `ModelLoop::every` timers and an MCP pump that ticks while idle; every 100 ms plans are freed, `fx.failed` is emitted unasked, a device change restarts the engine (`engine.restarting`, back-off, `engine.error` after 30 s; position and loop kept), autosave to `<file>-data/autosave-<n>.json` (newest 5; `autosave_available` on open) |
| P4.21 | worst-case block cost at 64 frames: why one Orrery chain xruns in the callback (Quire without anticipation and REAPER) while the average load is far below a core — per-block timing histogram for the B1 session (note-on/off bursts at chord changes, CPU clock ramp under `powersave`) | queued |
| — | review round 14 (transport seqlock, VST3, commands) | **fixed** (reviewer, Hermes, on the orchestrator's decisions): the renderer's transport read is bounded; a VST3's controller is never called on the audio thread (plain automation lanes compile into the normalized domain; `Instance::paramsNormalized/normalizedParam/plainParam`, bridged); settled VST3 bus widths, reset() ends sounding notes; render path symlink check; `track.move`; checked ids; item.set conflicts |

## Phase 5 — The DAW GUI (spec `docs/specs/gui.md`, ADR-0005)

| ID | Task | State |
|---|---|---|
| P5.1 | app skeleton: `<slug>` binary, SDL3 X11/offscreen window, GL 3.3 + NanoVG, the bus served from the UI thread (ModelLoop inside the SDL loop), offscreen snapshots, cold-start bench | **done** (orchestrator) |
| P5.2 | toolkit core: nodes, stack layout, damage repainted into a cached framebuffer, input/focus, theme tokens (dark/light, JSON), golden images and scripted UI tests | **done** (orchestrator) |
| P5.3 | bulk GL layers (strips, instanced rects), peak pyramids (worker, disk cache), the track lanes view, 64-track bench | **done** (orchestrator): release cold start 79–92 ms, 64-track p95 0.45–0.59 ms UI-thread CPU, idle 0.3 % (`docs/benchmarks/gui.md`); `model::itemSpan` shared with the engine. Meters → P5.3c |
| P5.3c | per-strip meter telemetry aligned to what is heard (anticipation) + meter widgets | **done** (Hermes `notation`, reviewed by the orchestrator: `MeterBank`, two SPSC rings, readings held until heard, master == offline render bit-for-bit, rtsan/tsan clean; strip meters pre-encode incl. pan law, deferred slot reuse, drops in engine.status); widgets and the GUI feed (`MeterFeed`) by the orchestrator |
| — | toolkit widgets: TextField (UTF-8, IME, clipboard, secret), ScrollBar, virtualized List, popups/menus, TextBlock, ScrollView | **done** (orchestrator) |
| P5.4 | arrange view, read-only (ruler, tempo/markers, rows shared with the TCP, fades, playhead overlay, zoom/scroll) | **done** (Hermes `gui`, reviewed by the orchestrator) |
| P5.5 | track control panel + transport bar | **done** (Hermes `gui`, reviewed by the orchestrator): pooled TCP rows sharing the arrange geometry (name with inline rename, type/parentage, M/S/arm/monitor, input, fader, pan, meter), "+ strip" (Channel/Buss/Master), transport bar (play/stop, record, loop, punch, position); idle CPU work (labels written only on change, meters at rest cost nothing) |
| P5.6 | editing gestures | **done** (orchestrator): select (click/Shift/Ctrl, marquee, right-drag marquee), move across channels (snapped to the zoom's grid; Shift free), Ctrl-drag copy, trim (left edge keeps the material), slip (Alt), stretch (Alt on the right edge: length + play rate), fade handles (overlaps from a move/copy/trim crossfade automatically), take badge, double-click opens MIDI, file drop, a double-click on empty channel space (or Item > New MIDI item) makes an empty MIDI item and opens it, item clipboard (Ctrl+C/X/V, Ctrl+D; `item.paste` copies items as they were at the copy's version, so cut+paste is exact); ghost on the overlay, one transaction per gesture. Split is the S action. Take comping: an item with several takes shows them as lanes (click a lane to hear it, swipe a range to comp it in). Fade shapes: Ctrl+click a fade handle cycles them (`item.set {fade_in_shape, fade_out_shape}`) |
| P5.7 | mixer | **done** (orchestrator): grouped strips (parentage), faders/pan/meters/mute/solo, Console 9 toggle, horizontal scroll, coalesced drags |
| P5.8 | FX and plugin GUIs | **done**: FX chain window, plugin browser (favourites, stock first, each plugin's file, shadowed copies marked), parameter panel, per-FX state badge with Restart (Hermes `gui2`, reviewed); plugin editors in their own windows, CLAP gui + VST3 IPlugView on X11 with the posix-fd/timer loop (orchestrator). Editors of isolated and bridged (VST3) plug-ins embed from the bridge process (bridge protocol v5); the GUI app finds the bridge executable. Per-FX pins on strips with auxiliary channels (ADR-0020); take FX in the FX window |
| P5.9 | routing | **done**: routing matrix (sources by parentage, destinations incl. keys, blocked cells say why, tap/level/pan/mute details), `track.set {parent_send}` toggle (orchestrator); drag a strip's "out" onto another strip (or its lanes) to send, a click opens the matrix at the button (Codex `ruler`, reviewed). Channel maps, mono and polarity in the matrix (`route.add/set {channels, mono, phase_invert}`), including the auxiliary pairs both ends have (ADR-0020) |
| P5.11 | MIDI editor | **done**: piano roll (draw, move, resize, velocity, quantize), velocity lane, CC lane (1, 7, 10, 11, 64; draw/erase), notation panel, `midi.notes`/`midi.edit` (orchestrator); event list — every note and CC, text-edited cells, Delete, + Note (Codex `events`, reviewed) |
| P5.12 | render dialog | **done** (Hermes `gui2`, reviewed): source/bounds/format/destination, live dry-run file names, jobs, add to project, project render settings as defaults |
| P5.13 | recording | **done**: arm/monitor/input per strip, the input picker from `engine.inputs` (one engine input per physical JACK port, MIDI devices), record/loop/punch on the transport bar (Hermes `gui`, reviewed); metronome — a click on every meter beat in the live output only, accented bars, `transport.metronome`, the transport bar's "click" (orchestrator); count-in (1/2/4 bars, also before playback if asked), the punch range in the ruler's time row, the record folder shown while armed |
| P5.14 | automation lanes and modes | **done** (orchestrator): volume/pan/mute and plugin-parameter lanes under their strip (the engine's segment shapes; a plugin parameter's own curve and units), click adds / drag moves / Alt+click deletes a point (one undo step), header with mode, on, hide; the strip's env menu and the FX parameter panel's "env" create/show/hide/remove; touch/latch/write record `track.set`/`route.set`/`fx.param.set` changes while playing (latch/write hold to the stop; one "Automation written" step); `env.set {visible}`. Automation items with pools (`autoitem.*`; insert/duplicate/unpool/loop/delete in the lane, header moves, right edge resizes, points edited inside); take envelopes on items |
| P5.10 | actions (every command + bound/macro/built-in), keymaps (sections, sequences, user deltas), the actions list (search, prompts, Ctrl+B binding), menus and toolbar (menus.json), `gui.selection` | **done** (orchestrator); the toolbar and menus edited in the app (Ctrl+T / Ctrl+M in the actions list, a tool's right-click menu), the mouse-modifier map (mouse.json, Edit > Mouse modifiers), named screensets and per-track heights |
| P5.15 | agent panel | core **done** (orchestrator): Anthropic + OpenAI-compatible streaming, tool loop over the MCP surface + `skill_read` + `listen`, work on `agent/<client>/<n>`, diff / A/B / accept / reject, `Origin::Type::Agent`; panel UI **done**: chat, tool rows, listening cards, named diff, A/B, accept/reject, endpoint settings (0600) |
| — | `MediaSource::hash` is never filled (no SHA-256 in the tree); peaks key by path meanwhile | **done** (Hermes `notation`, reviewed): sha256 of the file bytes, item.add reuses a source by content, pre-hash projects converge (path + header, hash backfilled) |
| — | history dock | **done** (orchestrator): versions, branches, jump, the command log (`log.list`) with origins |
| — | ruler editing | **done** (Codex `ruler`, reviewed): a ruler drag sets the loop range (snapped), markers (M; drag, rename, Alt+click), regions (Shift+R over the loop range; move, resize, rename), tempo points (double-click edit/add; Alt+click toggles a ramp) |
| — | FX chain presets | **done** (Codex `events`, reviewed): `fx.chain.save/load/list` (supple.fx-chain/1.0 in `$XDG_CONFIG_HOME/<slug>/chains`, semantic patches or base64 state, load = one undo step, append/replace), Save/Load in the FX window. Track templates: `track.template.save/load/list` (a strip's settings and both chains; Track menu) — orchestrator |
| P5.16 | visual identity pass | **done** (orchestrator) after Nate's review (HUMAN_REVIEW #17): warm (orange, default) and ink palettes × dark (default) / light, switched from the View menu and saved; track colours (a stripe in the TCP and the mixer, picked from the track's right-click menu — Rename, Color…, Delete, in the TCP and the mixer — or Track > color…; `track.set {color}` validated); masters pinned right in the mixer with a heavier spine; gui.md §7 updated. Renders: `tools/gui/mockups.py` |
| P5.17 | performance gates and the workflow-gap log | gap log **done** — all 40 workflows done as of p5-gaps (2026-10-04; `docs/research/gui-workflow-gaps.md`); bench numbers in `docs/benchmarks/gui.md` |
| — | p5-gaps (2026-10-03/04) | **done** (orchestrator): count-in, punch range, route channel maps, timebase, screensets, FLAC + second file + render queue, take FX and envelopes, automation items, mouse map and chrome editing, bridged editors, auxiliary channels and plug-in pins (ADR-0020) |

## Codebase review and cleanup (2026-10-04)

A full review of the existing DAW by four Codex workers (`gpt-6.1-sol`, one
worktree and area each: engine, command layer, GUI, plug-in hosting), every
batch reviewed and integrated by the orchestrator; per-area reports with
each fix's trigger, test and verification live in the review worktrees.
Roughly 250 fixes, most with a regression that fails without them. Notable:

- **Engine:** plug-in automation is independent of the block partition; a
  recording overflow keeps time; bridged FX are never run from the device
  callback (passed through with a warning on live nodes); post-fader keys
  ignore solo (ADR-0018); held notes, edge delays and aux delays carry
  across plan swaps; NoteEnd voice addressing; request_process wakes
  plug-ins; params rescan/clear re-resolve lanes (envelopes never deleted);
  recordings commit at stop and are hashed in the background (ADR-0014).
- **Command layer:** undo/redo reload the running engine; each invocation in
  a batch is atomic, and savepoint failures refuse or roll back (ADR-0007);
  take copies clone FX/envelopes/automation; merges remap ids and see
  project metadata; history reclaims unreachable versions and has a 1 GiB
  budget that squashes the oldest steps (ADR-0008); project files keep
  unknown top-level sections and `extensions`, UI state lives only in the
  `<name>.ui.json` sidecar, `groups` is preserved but not implemented
  (ADR-0009); raw and derived numeric checks on every edit/transport/render
  argument; Lua finalizers refused (ADR-0006).
- **GUI:** callback and dispatch lifetimes; stale gestures after external
  edits; fades drawn with the engine's shapes (`model/fade_shape.h`); MIDI
  thumbnails use playback's source mapping; change-scoped arrange
  invalidation (an unrelated FX edit in a 200-track session repaints
  nothing); wheel zoom under the pointer.
- **Hosting:** state loads are barriers for queued edits; bridge requests
  have one deadline and protocol v6 carries error details, full output
  events, wake and clear; supervised scanning in a scan-only bridge child
  with a success/failure cache (ADR-0013); plug-in editor edits reach the
  model (one undo step per gesture, no echo; read-only outputs are display
  only).

Known limits after the review: overlapping gestures on a plain (opaque
state) plug-in share their state blob; the history budget's size estimate
walks shared payloads per version and squashed steps keep large invocation
arguments; track groups are not implemented; a live (pipelined) bridge mode
does not exist yet, so bridged FX are passed through while monitoring.

## GUI workflow batch (2026-10-05)

Nate's list, done by three Claude Code workers (mixer, tracks, midi), reviewed
and merged by the orchestrator (main 330a4da):
- Top bar: only Undo, Redo, Add channel, Actions list (transport buttons live in
  the transport row).
- Track list: busses after channels under each parent (`views::displayChildren`;
  the mixer groups from the same order); drag an empty part of a row to reorder
  within its type (`views::reorderIndex` → `track.move`), shared with the mixer.
- Wheel zoom anchors at the project cursor by default; `wheel.zoom` mouse setting
  (View > Wheel zoom anchor) switches back to the pointer. Ruler labels are clipped
  (they overlapped the history panel's "Branch: main").
- Mixer: masters look like other strips; small fx button top right; resizable by its
  top edge (only the chain grows, double-click = 5 plug-ins, saved in the UI
  sidecar); drag plug-ins to reorder (`fx.move`) or onto another strip to copy them
  with their live settings (new `fx.copy`); drag strips to reorder.
- MIDI editor: resizable by its top edge (double-click resets, saved); Alt+wheel
  zooms the key rows. Both docks share `panel_height.h`'s clamp.
- Open: no autoscroll during drags; no resize cursor (the UI toolkit has none);
  plug-ins drop onto mixer strips only, not track-list rows; `fx.copy` does not
  copy envelopes or key routes.

## Strips, routing and monitoring batch (2026-10-06)

Nate's next list, done by the orchestrator:
- Monitoring needs the arm (REAPER's rule): `on` while armed, `auto` while armed
  and stopped or recording (tape style, decided per block in `processNode`).
  New channels monitor `on` by default; Edit > Preferences (new,
  `preferences.json`) switches the default and holds the wheel zoom anchor.
- Names: track.add numbers new strips `ch_N`, `buss_N`, `mstr_N` (tr_0 stays
  "Master"); `model::trackLabel` gives every view (and `route.matrix`) the same
  label. The routing matrix lists strips in the views' order, masters last.
- Strip controls, the same in the TCP and the mixer: pill M/S in their own
  colours, a red arm disc, a speaker for monitoring (outline/solid/green),
  "fx" (chain) + "+" (the plugin browser alone, with its own Close; no count).
  The mixer gains the input button, "out" (click: matrix; drag: a send) and
  rename by double-click; a double-click on a chain's empty part opens the browser.
- A control on a selected track acts on the whole selection (`gui::SelectionEdit`):
  toggles, monitoring and input set; fader and pan drags move relatively; one undo step.
- A click elsewhere ends a rename and keeps it; Space never presses a button
  (play/stop only); R records, L loops; Alt+D the mixer; Alt+E the MIDI editor.
- FX chain: rows reorder by drag (no arrows). Arrange: the marquee published its
  selection only on release, so the app's per-frame selection sync undid it.
- Plug-in latency modes (Ferric, Plateau "Low Latency"): a restart a plug-in asks
  for while the engine runs is now served (the plan takes it off the executors,
  the same instance restarts, the new latency is compensated); VST3
  kLatencyChanged reactivates as the SDK requires. VST3 builds made with
  clap-wrapper only forward the request while their editor is open.
- VST3 editors: the view's size is asked before it is attached, so
  clap-wrapper builds open at their CLAP size (they sized from 0x0 to their
  minimum). A send drag onto a strip the track already sends to adds nothing.
- Multi-output plug-ins (ADR-0021): a CLAP plug-in's extra output ports and a
  VST3's auxiliary output buses reach the engine (bridge protocol 8); strips
  have up to 128 channels; `fx.add` gives an instrument's outputs new channels
  (`pins.out` past the main pair); the routing matrix has a row per pair
  ("3/4 Kick") that sends it on. RT ≡ offline, in process and bridged.
- LSP Multi-Sampler (Nate's report): plugins now run while the transport is
  stopped (they were processed only when monitoring or on request_process, and
  LSP loads samples only in process()); play re-locates and resets the
  renderer's share so RT ≡ offline holds. Up to 128 ports per direction (the
  x24/x48 DirectOut variants have 25/49 and were refused at 16) and 128 track
  channels. A plain plugin's state is captured on mark_dirty and before every
  save (sample paths were never saved).
- Projects with a strip over 8 channels saved but would not reopen (the
  project schema still capped `channels` at 8); fixed, with a save/reopen check.
- Strip polarity and mono (ADR-0022, amends ADR-0019 §5): `phase_invert` and
  `mono` on every strip, after the inserts and before the fader (never between
  a Console 9 encode and decode), ramped like mute. "Ø" and "mono" pills on
  the TCP's level row and, in the mixer, in one row with monitoring above the
  pan (the arm moved up under the plug-ins); they follow the selection.
- Plug-in editors reopen where they were last and at their last size
  (resizable editors), per effect, kept in the UI file's `plugin_windows`
  section so they survive reopening the project.

## Team (Hermes workers via Herdr; Codex retired)

Roster + briefs: `~/Projects/quire/.orchestrator/` (git-excluded). Four Codex
workers (`gpt-6.1-sol`), one worktree each, take well-specified
implementation; the orchestrator designs, reviews every branch and
integrates. Codex's weekly budget is nearly spent (2026-09-30): these are
their last tasks, and the orchestrator finishes any that run out.

- **contour** — EQ, reverb, LLM eval harness (landed). Done.
- **margin** — analyzer, gate, delay, `libs/console9`, Orrery performance pass (landed). Done.
- **press** — compressor/limiter, transient shaper, display-name cleanup, Console 9 typed strips (landed). Done.
- **trim** — utility, saturation, REAPER check, B1/B2 benchmark tooling (landed). Done.

Hermes, two profiles (models per Nate, 2026-09-30; his default profile is never edited):
`quire-work` — `deepseek/deepseek-v4.1-flash` via OpenRouter, for hermes-team
work agents (`herdr agent start <name> --kind hermes --pane <id> -- -p quire-work`);
`quire-eval` — `meta/muse-spark-1.3-contributor` via OpenRouter, the model
under test. The eval harness reads only its model identity and key reference
from it and runs every task in a fresh isolated Hermes home, so evaluations
and work agents can run at the same time.

- **notation** — notation pipe, plugin reset, engine meters, media hashes (landed).
- **scoring** — scoring, optimizer tuning, describe fix, eval provenance (landed). No reruns (Nate, 2026-10-01).
- **gui** — arrange view, TCP + transport, input picker (landed).
- **gui2** — FX/plugin GUIs, render dialog, slider, FX badges (landed).
- Codex (`gpt-6.1-sol` only, launched with `--no-daemon`): **events** — MIDI event list, FX chain presets; **ruler** — ruler editing, drag-to-send (landed). The 2026-10-04 review team — **engine**, **commands**, **gui**, **host** — landed; weekly usage nearly spent again (resets 2026-10-11).

OpenRouter credits ran out on 2026-10-01 (~13:20): every Hermes worker is
paused; the orchestrator finished their in-flight work (harness commit,
merges) and continues alone until Nate adds credits.

Test labels: `slow` (dev/gcc/asan only), `tsan-skip` (AGENTS.md). Local CI
is ~25 min per full run and grows with the optimizer/analysis suites.

Not yet fixed from review round 2 (minor, recorded): describeFlat skips hidden
params in Full detail; core-params formatting of extreme magnitudes (>6
decimals); Rng::uniform extreme endpoints; RtHandoff forced-schedule tests.

## Blocked / waiting on Nate

See `HUMAN_REVIEW.md` (name veto, license choice; FYIs on location, kernel,
fonts). Nothing is blocked.
