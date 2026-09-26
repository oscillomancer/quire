# Spec: the parameter optimizer and long-running jobs (P3.3)

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
  generations.
- **Search space**: every parameter is mapped to its **normalized** domain
  [0, 1] (core-params curves, ADR-0010), so a log-frequency and a linear
  percentage are equally easy to search. Initial mean = current values;
  initial σ = 0.2 (option); bounds handled by mirroring into [0, 1]
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
  1e-4), cancellation.
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
| `params` | list of parameter keys, each optionally `{key, min, max}` to narrow the search range (physical units, converted to normalized sub-ranges); or `"auto"`: all continuous, modulatable, non-readonly params **of the modules present in the patch**, excluding output level (so the optimizer cannot "win" by getting louder) |
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
  - `ltas`: RMS of the 1/3-octave LTAS difference after mean removal (dB),
    weight 1.0, scale 3 dB;
  - `bands`: RMS of the named band level differences (dB), weight 0.5,
    scale 2 dB;
  - `centroid`: |log2(c/c_ref)| in octaves, weight 0.5, scale 0.25 oct;
  - `envelope`: |attack − attack_ref| / max(10 ms, attack_ref) and the same
    for decay20 (and release when a note-off is known), weight 1.0, scale 0.3;
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

These defaults are a starting point, not truth: the evaluation suite (P3.4)
tunes them, and every run reports the per-term breakdown so an agent (or
Nate) can see *why* a candidate won.

## 4. Jobs: long-running actions on the bus

Optimization (and later rendering, analysis of long sessions, plugin scans)
can outlast a tool call. The command bus gains a generic job facility:

- An `Action` command may return `{"job": "jb_<n>", "status": "running"}`
  instead of its result; the host runs it on a worker thread.
- `job.status {job}` (q) → `{status: running|done|failed|cancelled,
  progress: {fraction, message, best_loss?, evaluations?}, result?, error?}`.
- `job.wait {job, timeout_s ≤ 30}` (q) → like status, but blocks up to the
  timeout for completion (so an MCP client can poll without spinning).
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
    "evaluations": 280, "seconds": 21.4, "stopped": "stagnation",
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

A synthetic end-to-end test with the probe synth: render a hidden target
patch (cutoff 2.4 kHz, resonance 40 %, decay 180 ms), start from defaults,
optimize those three params with the reference target, and require the
result within 10 % of each hidden value (normalized domain) within 300
evaluations — deterministic under a fixed seed, and identical on 1 and 8
threads. A profile-only test (hit `spectral.centroid_hz.median` 1500 ± 50 and
`envelope.attack_ms` 5 ± 2).

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
