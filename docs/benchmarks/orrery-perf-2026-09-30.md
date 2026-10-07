# Orrery performance pass, 2026-09-30

Base: `508a229`. Machine: Intel Core Ultra 5 225F, Clang 22.1.8,
Release `-O3 -ffp-contract=off`, baseline x86-64 (no native/AVX flags).
All timing runs use `taskset -c 0`; no governor or system settings changed.
48 kHz, 256 frames, eight notes × three unison, normal quality. Median of
three runs, with the benchmark's 200-block warm-up before each run.

The shared workstation has substantial scheduling interference. The final
benchmark also reports process CPU time (`std::clock`, with no worker threads)
to distinguish work from time descheduled. The original wall-time measurement
remains the first column; neither measurement reaches the 3% target.

## Measurements

These are successive measurements, not isolated estimates of each component.
Except the initial 30-second baseline, each reference run renders 90 seconds
of audio total (three 30-second trials).

| Step | Reference wall % | Reference CPU % |
|---|---:|---:|
| Initial base (30 seconds) | 14.79 | not recorded |
| Specialized classic VA/sub loops and bounded phase reductions | 11.31 | not recorded |
| Double ladder lanes, automatic vectorization experiment (discarded) | 12.18 | not recorded |
| Double ladder lanes, forced two-wide experiment (discarded) | 12.76 | not recorded |
| Four float ladder lanes, double phase/coefficient ramps | 10.73 | not recorded |
| Cached parameter starts and level/gain conversions | 10.03 | not recorded |
| Constant coefficient/ramp fills | 10.82 | not recorded |
| Per-curve drive blocks, zero-bias specialization | 10.32 | not recorded |
| Cached pitch/cutoff conversions; muted drift advances without cosine | 9.46 | 9.34 |
| Final, warning-clean drive kernels | 10.66 | 9.11 |
| Base, same current harness immediately after final | 16.26 | 13.77 |

The final paired CPU measurement is a 33.8% reduction. Wall times vary much
more; a subsequent 30-second variants run gave reference 12.41% wall / 9.13%
CPU. Variants in that run:

| Variant | Wall % | CPU % |
|---|---:|---:|
| No FX | 9.71 | 8.55 |
| No drive | 9.33 | 8.21 |
| SVF instead of ladder | 9.60 | 7.83 |
| No filter | 8.62 | 6.87 |
| One oscillator | 15.99 | 6.81 |
| Unison 1 | 5.99 | 5.06 |
| Eco | 8.51 | 5.86 |
| High | 25.18 | 16.57 |

Variants change the signal and downstream curve workload, so subtraction is
not an exact component timing. In particular, the one-oscillator wall result
shows why CPU time is useful on this shared machine.

## Profile and stopping point

`perf` is unavailable. An independent Release build with `-pg`, sampled by
`gprof`, identified the base's staging (~40%), grouped filters (~24%), and
control evaluation (~17%) as the main costs. Profiling instrumentation changes
inlining and adds call overhead; its timings are not acceptance measurements.

The final profile attributes about 43% to staging/classic oscillator kernels,
21% to the lane ladder, 16% to targets/parameter reads, and 9% to drive.
Clang vectorization diagnostics and disassembly confirm four-wide float
ladder arithmetic and two-wide double drive arithmetic, using baseline SSE.
No intrinsics or fast-math were added; GCC and other architectures retain
portable scalar code when their compiler does not vectorize these loops.

This pass stops at the structural limit of the existing voice representation.
Oscillator phase still recurs in double precision separately for each voice;
staging writes and rereads oscillator, routing, coefficient and amp buffers
for every tick segment. Filter lanes gather those separate buffers, and a
three-voice group leaves one SIMD lane unused. Control evaluation also walks
the modulation/parameter graph independently for each unison member.

Reaching 3% needs a persistent structure-of-arrays voice bank, oscillator lanes
across voices (while preserving double phase accuracy), and a fused routing /
filter / amp path that avoids these intermediate buffers. Sharing note control
work also needs explicit dependency analysis: random, drift, voice LFO/step
RNG, MPE, and routed modulator parameters can distinguish unison members.
Blindly sharing their state would change patches and later automation. Those
layout and scheduling changes are left for a separate structural pass.

## Numerical and deterministic acceptance

The optional `orrery-reference-bench` compiles the DSP sources from before
this pass (from a git archive) with the current capture harness. Its capture
was byte-equal to the baseline captured before the DSP was edited.

`--dump-guide` captures the §9 benchmark patch followed by every JSON block
in `skill/guides/sound-design.md`: eight seconds each, 48 kHz, 128 frames,
seed from each patch, eight-note chords at non-tick sample offsets, retriggers,
note-offs, and release/tail. Format: native IEEE float32, 128 L samples then
128 R samples per block, 3000 blocks per patch; no header. Both captures must
be produced on the same machine from the same guide/harness.

| Patch | Residual dB relative to baseline stereo RMS |
|---|---:|
| Reference | -137.84 |
| Anatomy | -132.13 |
| Warm pad | -135.72 |
| Plucky bass | -128.99 |
| Acid bass | -123.57 |
| Sub bass | bit-identical |
| Supersaw | bit-identical |
| Metallic bell | bit-identical |
| FM electric piano | bit-identical |
| Noise snare | bit-identical |
| Vowel lead | bit-identical |
| String ensemble | bit-identical |

All pass -80 dB; no chaotic-feedback exemption is needed. The older diverse
`--dump` fixtures also pass (worst -130.02 dB), covering cyclic FM/AM, sync,
stereo formant/comb, fold/hard drive, legato, eco/high, and steps.

Tests compare scalar/grouped ladders through changing group widths, specialized
oscillator phase/ramps, scalar/block drive with bias ramps, signed-zero and
sub-ULP constant fills, and drift RNG after muted advancement. Every guide
example is bit-identical across host blocks 1, 7, 256, 4096, including mono →
stereo → centre oscillator-pan automation. Existing engine tests retain the
FM/AM per-sample modulator level and other block-invariance checks.

## Reproduce

From this branch's checkout (all build data stays under `build/`):

```sh
mkdir -p build/orrery-baseline-src
git archive 508a229 plugins/orrery/engine | tar -x -C build/orrery-baseline-src
cmake --preset release -DSUPPLE_ORRERY_BASELINE_DIR="$PWD/build/orrery-baseline-src/plugins/orrery/engine"
cmake --build --preset release --target orrery-bench orrery-reference-bench orrery-engine.tests -j6
taskset -c 0 build/release/plugins/orrery/engine/orrery-bench --reference 90
taskset -c 0 build/release/plugins/orrery/engine/orrery-reference-bench --reference 90
taskset -c 0 build/release/plugins/orrery/engine/orrery-bench 30
build/release/plugins/orrery/engine/orrery-reference-bench --dump-guide build/orrery-reference-guide.raw skill/guides/sound-design.md
build/release/plugins/orrery/engine/orrery-bench --dump-guide build/orrery-final-guide.raw skill/guides/sound-design.md
python3 plugins/orrery/engine/bench/compare_renders.py build/orrery-reference-guide.raw build/orrery-final-guide.raw
```

The comparison script fails for nonfinite/silent/short/oversized captures or a
residual above -80 dB; it has no Python dependencies.
