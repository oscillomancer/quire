# Spec: the parameter optimizer and long-running jobs

**Status:** Implemented (2026-09-26); amendments at the end.
**Code:** `libs/optimize` (`supple::opt`: CMA-ES, bounded, deterministic;
no audio knowledge), `host/sandbox` (the `optimize.*` commands, loss
functions over core-analysis, the render worker pool), `libs/command-bus`
(the job pattern, §4).

The division of labour this project rests on: **the LLM chooses structure**
(which modules, how they route, which parameters matter, a musical phrase
that exercises the sound) and **the optimizer tunes numbers** (the
continuous parameters) against a measurable target. The LLM is bad at
nudging twenty coupled knobs by ear; a derivative-free optimizer with a good
loss is good at exactly that.

## 1. Algorithm: CMA-ES (`libs/optimize`)

- (μ/μ_w, λ)-CMA-ES as specified in N. Hansen, *The CMA Evolution Strategy:
  A Tutorial* (arXiv:1604.00772) — implemented from the paper's equations
  (default strategy parameters: λ = 4 + ⌊3 ln n⌋, μ = ⌊λ/2⌋, log weights,
  cumulation and damping constants as in the tutorial's Table 1), with
  active covariance update off by default (option), and eigen decomposition
  of C via a small Jacobi solver (n ≤ 64 here) every ⌈λ / (c1 + cμ) / n / 10⌉
  generations. In `Options`, nonpositive `lambda` selects the default;
  positive values are clamped to a minimum population of 2.
- **Search space**: every parameter is mapped to its **normalized** domain
  [0, 1] (core-params curves, ADR-0010), so a log-frequency and a linear
  percentage are equally easy to search. Initial mean = current values;
  initial σ = 0.2, or **0.05 when the target is a reference** (the agent's
  patch is already close to the answer — measured in
  `docs/benchmarks/optimizer/`); bounds handled by mirroring into [0, 1]
  (reflect; the unmirrored genotype stays in the distribution). Integer and
  enum parameters are **not** optimized (structure belongs to the LLM); ints
  may be opted in and are rounded after mirroring.
- **Determinism**: all randomness from a seeded xoshiro256** stream (core-
  base `supple::Rng`, added with this task); candidate generation order
  fixed; parallel evaluation writes results by candidate index; identical
  seed + inputs → identical trajectory and result on any thread count.
- **Stopping**: evaluation budget (default 300), wall-clock budget (default
  60 s), target loss reached, stagnation (no improvement > 1e-4 relative in
  best loss over 10 + ⌈30 n / λ⌉ generations), σ collapse (σ·max(√diag C) <
  1e-4), cancellation. Non-finite losses rank last and do not reset the
  stagnation counter; the first finite best loss counts as an improvement.
- API: `class Cmaes { Cmaes(n, mean, sigma, seed, options); std::vector<Vec>
  ask(); void tell(std::span<const double> losses); bool done() const;
  Result best() const; }` plus a driver `minimize(f, x0, options, onProgress)`
  that evaluates `ask()` batches through a caller-provided parallel map.
- Tests: standard functions (sphere, ellipsoid, Rosenbrock n = 10, Rastrigin
  with restarts off) reach known tolerances within published evaluation
  counts (±50 %); determinism across thread counts; bound mirroring.

## 2. What is optimized (sandbox)

`optimize.start` args:

| field | meaning |
|---|---|
| `instance` | the plugin instance (semantic extension required) |
| `params` | list of parameter keys, each optionally `{key, min, max}` to narrow the search range (physical units, converted to normalized sub-ranges); or `"auto"`: all continuous, modulatable, non-readonly params **of the modules present in the patch**, excluding output level (so the optimizer cannot "win" by getting louder). `"auto"` searches a **window around each parameter's current value** — ±2 octaves for frequency, time and pitch-interval quantities, ±25 % of the range otherwise — because the agent's patch is the starting point, not a random draw; an explicit `{key, min, max}` keeps exactly the range you give, and an explicit key without bounds keeps the whole range |
| `target` | one of: `{reference: <render id or file>}`, `{profile: <supple.analysis-profile document>}`, or both (profile terms added to the reference terms) |
| `phrase` | notation text rendered for every candidate (default: a single `C3:w` for 2 s + tail); must be identical to the one the reference was rendered with when matching a reference |
| `render` | `{seconds?, tail_s?, sample_rate?}` (defaults: session settings, tail 1 s) |
| `budget` | `{evaluations, seconds}` |
| `seed` | integer (default 0) |
| `weights` | optional per-term weight overrides (§3) |
| `apply` | bool (default false): merge the best patch into the instance when done |

At most 32 parameters per run (CMA-ES scales poorly beyond and the LLM
should narrow the problem instead); more is an error with a hint.

## 3. Loss

Each candidate: set the parameters (semantic `set_patch` merge on a
worker-owned plugin instance cloned from the target instance's state),
render the phrase offline (deterministic), analyze with the Standard
report, compute:

- **Reference terms** (when matching a reference). The candidate is first
  loudness-matched to the reference (integrated LUFS; gain applied to the
  analysis input, not stored) so that level differences do not dominate
  timbre terms. Then:
  - `ltas`: RMS of the 1/3-octave LTAS difference over bands active on either
    side; missing bands are floored at −100 dB. Level offset is estimated only
    from bands active on both sides (zero if none), so silence is a mismatch.
    Weight 1.0, scale 3 dB;
  - `bands`: RMS of the named band level differences (dB), weight 0.5,
    scale 2 dB;
  - `centroid`: |log2(c/c_ref)| in octaves, weight 0.5, scale 0.25 oct;
  - `envelope`: `|log2((attack + 10 ms) / (attack_ref + 10 ms))|` and the same
    for decay20 (and release when a note-off is known), weight 1.0, scale 0.43
    (≈ 0.3 / ln 2, so for references well above 10 ms the term matches the old
    ratio `|c − r| / max(10 ms, r)` at scale 0.3); a value present on one side
    only scores distance 1.43 at that scale (≈ 11.1, as before);
  - `harmonics` (when both pitched): RMS difference of the first 8 harmonic
    levels (dB), weight 0.5, scale 3 dB; `inharmonicity` absolute
    difference, weight 0.3, scale 0.01;
  - `stereo`: |width − width_ref|, weight 0.2, scale 0.1;
  - `flatness`: |f − f_ref|, weight 0.3, scale 0.05;
  - `flux` (movement): |median flux difference|, weight 0.3, scale 0.02.
- **Profile terms**: core-analysis `evaluate` normalized distances, each
  squared and weighted by the profile's weights.
- **Total**: `Σ w_i · (d_i / scale_i)²`, reported with every term's
  contribution. Failed renders (plugin error, non-finite output) get loss
  `+inf` and are counted.

These defaults are a starting point, not truth: the evaluation suite
(`specs/eval-harness.md`) tunes them, and every run reports the per-term
breakdown so an agent (or a person) can see *why* a candidate won.

## 4. Jobs: long-running actions on the bus

Optimization (and later rendering, analysis of long sessions, plugin scans)
can outlast a tool call. The command bus gains a generic job facility:

- An `Action` command may return `{"job": "jb_<n>", "status": "running"}`
  instead of its result; the host runs it on a worker thread.
- `job.status {job}` (q) → `{status: running|done|failed|cancelled,
  progress: {fraction, message, best_loss?, evaluations?}, result?, error?}`.
- `job.wait {job, timeout_s}` (q) → like status, but blocks up to the
  timeout for completion (so an MCP client can poll without spinning). The
  block is capped at 30 s: a larger `timeout_s` is accepted and clamped, and
  the result then carries `"timeout_clamped_s": 30` (0 returns at once).
- `job.cancel {job}` (a); `job.list` (q).
- Events `job.progress` (≤ 2 Hz) and `job.done`.
- Jobs are session-scoped, kept until the session ends or 100 finished jobs
  accumulate (oldest dropped).

`optimize.start` returns a job; `job.wait` returns the result when done:

```json
{
  "status": "done",
  "result": {
    "loss": {"initial": 14.2, "best": 1.7, "terms": {"ltas": 0.6, "envelope": 0.4, "bands": 0.3, "centroid": 0.2, "harmonics": 0.2}},
    "evaluations": 280, "failed": 0, "seconds": 21.4, "stopped": "stagnation",
    "patch": {"filters": {"filter1": {"cutoff": 1840.0, "resonance": 31.0}}, "envelopes": {"env2": {"decay": 212.0}}},
    "changes": [{"key": "filter1.cutoff", "from": 1200.0, "to": 1840.0, "unit": "Hz"}, …],
    "render": "rd_12",
    "comparison": { …core-analysis compare summary of best vs reference… },
    "applied": false
  }
}
```

`patch` is a merge patch relative to the instance's patch at start;
`render` is the best candidate's render (for listening and A/B).

## 5. Parallel evaluation

The sandbox keeps a pool of `min(λ, workers)` plugin instances per
optimization (clones of the target via state), one per worker thread, each
activated at the session rate. Candidate evaluation = set params → reset →
render → analyze, entirely on the worker. The pool is released at the end.
Memory: a few instances of the stock synth are cheap; third-party plugins
without the semantic extension are not supported by `optimize` (error with
the reason).

## 6. Tests

A synthetic end-to-end test with `orrery-headless.clap` (a saw through an
SVF with an envelope on its cutoff): render a hidden target patch (cutoff
2.4 kHz, resonance 60 %, decay 180 ms), go back to the defaults, optimize
those three params against the render with a fixed seed, and require the
result within 10 % of each hidden value (normalized domain) within 300
evaluations. A profile-only test hits `spectral.centroid_hz.median`
1500 ± 50 with the cutoff alone.

## Amendments (2026-09-26, implementation)

- `libs/optimize`: `Cmaes` (ask/tell) and `minimize()` as specified; options
  add `sigmaTolerance` (default 1e-4, the σ-collapse threshold — lower it for
  unbounded benchmarks). Tests: sphere/ellipsoid/Rosenbrock n = 10 reach
  1e-10 within the published budgets (+50 %), bounds, determinism, stopping.
- `optimize.start` arguments are flat: `instance`, `params`, `target`
  `{reference?, profile?}`, `phrase`, `seconds`, `tail_s` (default 1 s),
  `evaluations` (default 300), `max_seconds` (default 60), `seed`. `apply` is
  not implemented yet: the result's `patch` is `{"params": {...}}` (the
  semantic-params flat convention) for `patch.merge`. The command refuses to
  run inside a batch (a job cannot be rolled back); call it on its own.
- `"auto"` selects continuous, modulatable parameters marked active in the
  schema, excluding performance/placement parameters: `voice.*`, `*.pan`,
  `*_curve`, `*.delay`, `*.hold`, `*.velocity`, `*.fade`, `output.level`.
- Evaluation is single-threaded on one worker thread that owns a clone of the
  instance (cloned through its state). The best candidate's render is written
  by the worker under a render id reserved at start and joins the session's
  renders at the next command (`SandboxHost::makeContext`). Worker threads are
  joined through `cmd::Host::stopJobs()`, which `~Dispatcher` calls while its
  JobManager is alive.
- Candidate analysis uses `pitchFrames = 40` and skips mono-loss.
- An extra reference term, **`trajectory`**: RMS of log2(centroid/centroid_ref)
  over the aligned centroid tracks (≤ 100 points), weight 1.0, scale 0.1 oct.
  Without it a filter envelope's decay was not identifiable.
- End-to-end test with Orrery: a hidden cutoff/resonance/env2-decay patch is
  recovered within 10 % (normalized) from a 1 s phrase in ≤ 300 evaluations;
  a centroid profile is hit within 5 %; cancellation stops a job.

## Amendments (2026-09-26, review round 5)

- `optimize.start` snapshots the source plugin's state and identity on the
  model thread. The job worker creates, loads, activates, services, deactivates
  and destroys its clone on that same worker thread; a thread-checking CLAP
  fixture verifies the lifecycle.
- Reference WAV loading and analysis run in the job worker. Read or analysis
  errors fail the job with a structured error. A 30-second reference must not
  make `optimize.start` take 100 ms or more in the dev build.
- The worker checks `job.cancel` before every candidate, between rendering
  and analysis, and before the final write. Job completion atomically guards
  publication of the best render against cancellation; a cancelled job cannot
  publish a render afterwards.
- A profile is validated once at worker startup. Invalid profiles fail the
  job before any candidate. If every candidate fails, the job fails with the
  first structured cause rather than completing with infinite loss.
- Profile target weights are carried in `TargetResult` and multiply each
  squared optimizer term. Attack, decay20 and release each contribute a
  separately named, squared reference term.
- The best render record retains measured tail duration, latency, tail-cap
  status, chain and plugin log, so it can serve as a later reference.
- Starting a job joins and erases finished job threads on the model thread;
  active threads stay owned by the session until completion or shutdown.
- Semantic curve strings are parsed without exceptions. Unknown curves and
  non-finite or non-positive exponents return structured parameter errors.

## Amendments (2026-10-01, from the first evaluation runs)

- Envelope attack, decay20 and release use a **log-time distance**
  `|log2((c + 10 ms) / (r + 10 ms))|` at scale 0.43 instead of
  `|c − r| / max(10 ms, r)` at scale 0.3: the old ratio term was unbounded, so
  a 23 ms attack against a 0 ms reference scored 5877 and swamped every timbre
  term (≈ 1) in both the optimizer's landscape and the evaluation scorer.
  The 10 ms offset
  keeps a 0 ms value inside the domain; the new scale is ≈ 0.3 / ln 2, so a
  reference well above 10 ms scores what it did before. A value present on one
  side only keeps its old contribution: distance 1.43 at that scale ≈ 11.1.
- `compare` reports the same scored loss (`loss.total`, its terms, and the
  note-off used per side) through the shared `detail::referenceScore`, so an
  agent can see how close a render is without running the optimizer: the test
  is loudness-matched to the reference and both sides are analyzed with
  `detail::analyzeMatched`. The evaluation scorer's
  `eval-reference` subcommand calls the same function, so the harness number an
  agent can reproduce is the number it is judged by.
- `job.wait` accepts any nonnegative `timeout_s` and clamps the block to 30 s
  (reporting `"timeout_clamped_s": 30`) instead of rejecting the call.

## Amendments (2026-10-01, reference-search tuning)

- **A benchmark measures the search itself** (`tools/optimize-bench/`; every
  variant below is a row of `docs/benchmarks/optimizer/2026-10-01-tuning.md`):
  for every synth-reference task, set the
  task's `starting_patch`, render the hidden patch as the reference, and run
  `optimize.start` with 300 evaluations for three seeds in two modes — `"auto"`,
  and the changed modules' share of that list. On the code before
  this tuning (`baseline`) `"auto"` never improved on the start (median best
  21.8, 0 of 21 runs at or below `max_loss`) while the changed modules'
  parameters found the answer 9 times (median 3.0), as the evaluation runs had
  suggested. The search is deterministic — the same
  configuration and seed are bit-identical across runs — but its trajectory
  depends on the **order** of the requested `params` (the CMA stream is drawn
  per coordinate), so the benchmark records the order with every run.
- **Initial σ = 0.05 when the target is a reference** (0.2 for profile-only
  targets, unchanged). A σ sweep (`sigma-0.1`, `sigma-0.05`, `sigma-0.02`): the `"auto"` median
  best falls 21.8 → 8.8 → 4.4 → 3.2 and the changed modules' median 3.0 → 2.9
  → 1.1 → 2.2 for σ = 0.2 / 0.1 / 0.05 / 0.02, with passes 0 → 1 → 1 → 3 and
  9 → 9 → 10 → 10 of 21. 0.05 keeps the best median for the well-posed
  (changed-modules) search and is within one pass of σ = 0.02, which is why it
  is the reference default: one σ = 0.2 of a full range is a factor of ~8 on a
  log-scaled time, and the agent's patch is not a random draw.
- **`"auto"` searches a window around each parameter's current value**
  (`optimize_range.h`): ±2 octaves (×4 / ÷4) for frequency, time and
  pitch-interval quantities — the quantities a musician moves by a factor, and
  the ones the evaluation failures were about — and ±25 % of the range for
  everything else, clamped to the parameter's own range. On its own (σ = 0.2,
  `window`) it takes the `"auto"` median from 21.8 to 7.3; with
  the σ change (`window-sigma-0.05`) it reaches 3.3 with 3 of 21
  runs at or below `max_loss`, where σ alone reached 4.4 with 1 of 21. The
  changed-modules mode is untouched by it (explicit keys keep their range,
  which is also the escape hatch for an answer far outside the window).
- Tried and **not** kept, each measured (rows of the same table):
  - *a restart with σ/2 from the best point when a run does not improve on its
    start*: 0 of 42 runs reached the trigger once σ and the window were in
    place, and every loss was identical to the run without it
    (`restart`). The stagnation it targets is what the two
    changes above already fix; re-adding it is a one-line decision if a future
    target class stagnates.
  - *evaluating every candidate from the start state (re-loading the clone's
    state before each merge)*: all 42 losses were bit-identical to the
    accumulated-merge search and no candidate failed in any run
    (`fresh-state`). That rules out the hypothesized
    slot-removal failure mode for these tasks: candidate renders were already
    order-independent (`resetBeforeRender`, and plugin renders that depend
    only on their state after a reset).
  - *rescaling the `trajectory` term (0.1 → 0.2)*: it dominated the residual of
    failing runs (26–40 % of the loss) and rescaling it raised the pass count
    (13 → 16) and lowered the `"auto"` median (3.3 → 1.9,
    `trajectory-0.2`), but it made the search's *relative*
    progress worse (best/initial: `"auto"` 0.134 → 0.191, changed modules 0.035
    → 0.053, worse on 15 of 21 runs). The apparent win is a re-calibration of
    the score — the tasks' `max_loss = 1.0` is measured in this loss's units —
    not a better search, so the term keeps its scale. Rescaling it (or
    compressing its large distances) belongs with a re-calibration of the reference suite.
- `optimize.start`'s result also reports `"failed"` (candidates whose set,
  render or analysis failed), so a search that loses candidates is visible
  rather than looking like a badly scoring one.
