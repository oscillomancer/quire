# EQ review corrections — 2026-09-30

Branch `p3-eq`, follow-up to `4b8ac5a`. Original matched-filter math implementation;
no third-party code or dependencies added. The half-band code was removed.
Natural uses host-rate, double-state DF2T sections with a fixed 16-sample
coefficient grid during smoothing. Bells and shelves include a small fitted
correction SOS. Auto gain reuses current designs and skips unchanged curves;
control/UI response sweeps cache band designs. Minimum and linear remain intact.

The user clarified the accuracy requirement: bells Q ≤4, shelves Q ≤1,
f0 ≤10 kHz, through 20 kHz at 44.1/48 kHz, full ±30 dB gain range. Larger-Q
and near-Nyquist cases are measured approximation limits. Filter-specific fits
were explicitly approved after the requested three-point fit missed the bound.

## Exact commands and results

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --test-dir build/dev -R eq --output-on-failure
```

PASS: 3/3 — aggregate plugin tests, validator, engine. The engine includes
normal-domain accuracy, analog/process agreement, matched Butterworth cutoff,
block partition invariance, all-mode automation and silent tails, canonical
patch/state and RT/host contracts. Final logs:
`build/eq-revision-{config,build,tests}.log`.

```sh
cmake --preset release -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG'
cmake --build --preset release --target eq-bench -j6
build/release/plugins/eq/engine/eq-bench 120
build/release/plugins/eq/engine/eq-bench --accuracy
```

PASS: steady CPU below the ≤2× minimum budget, median of five CPU-time trials
per fixed non-flat patch, 120 audio seconds each, 48 kHz / stereo / 512 frames:

| Bands | Auto gain | Minimum (% core) | Natural (% core) | Ratio |
|---:|---|---:|---:|---:|
| 4 | off | 0.3103 | 0.2440 | 0.787× |
| 8 | off | 0.6081 | 0.4259 | 0.700× |
| 4 | on | 0.3431 | 0.3138 | 0.915× |
| 8 | on | 0.6675 | 0.4987 | 0.747× |

No benchmark is registered as a timing test. Patch definitions and measurement
loops are in `engine/bench/eq_bench.cpp`. CPU log: `build/eq-revision-cpu.log`.

Accuracy: 25 f0 values, gain in 3 dB steps, Q 0.1…4 for bells / 0.1…1 for normal
shelves; 2049 log and 2049 linear frequency probes plus resonance probes per
case. Both shelf directions have equal worst errors due to reciprocal design.

| Domain | 44.1 kHz maximum | 48 kHz maximum |
|---|---:|---:|
| Normal bells | 0.173604 dB | 0.059654 dB |
| Normal shelves | 0.254058 dB | 0.223204 dB |
| Resonant shelves, f0 ≤10 kHz, Q 1.2…24 | 17.219869 dB | 0.840856 dB |
| Bells near Nyquist, Q 4…24 | 1.500638 dB | 1.291749 dB |
| Shelves near Nyquist, Q 1.2…24 | 21.030647 dB | 16.180889 dB |

The last three rows are explicit limits, not ≤0.3 dB guarantees. The resonant
shelf fit tries the analog bump, rejects unrealizable/opposite-extremum fits,
and preserves damped analog zeros (without aliasing out-of-band zeros).
Full worst-case coordinates: `build/eq-revision-accuracy.log`.

```sh
tools/ci/run-all.sh
```

Completed once. EQ passed all six presets, including TSan and RealtimeSanitizer;
validators passed in dev/GCC. License and format passed. Whole-repository CI
returned 1 only because of the onboarding-documented sandbox socket failures
(`Operation not permitted`):

| Preset | Whole repository | EQ |
|---|---|---|
| dev | 40/45 pass | plugin, validator, engine pass |
| gcc | 40/45 pass | plugin, validator, engine pass |
| asan | 36/40 pass | plugin, engine pass |
| ubsan | 35/39 pass | plugin, engine pass |
| tsan | 34/38 pass | plugin, engine pass |
| rtsan | 36/40 pass | plugin, engine pass |

Failures: `control-protocol.tests`, `sandbox.mcp-proxy`, `engine.mcp-proxy`,
`mcp.tests`, plus `mcp.demo` in dev/GCC. No socket tests were changed.
Logs: `build/ci-<preset>.log`. Independent review also confirmed natural
0.24/0.43% versus minimum 0.31/0.61% for four/eight bands.

After verification, `build/{gcc,asan,ubsan,tsan,rtsan}` were removed. Dev,
the small release benchmark build, previews and verification logs remain.

```sh
python3 tools/license-check/check.py --root .
tools/ci/format.sh --check
git diff --check
```

PASS: 12 approved dependencies, formatting and whitespace.

```sh
build/dev/plugins/eq/eq-gui-preview 0 build/dev/eq-flat.ppm 1 flat
magick build/dev/eq-flat.ppm build/dev/eq-flat.png
build/dev/plugins/eq/eq-gui-preview 0 build/dev/eq-mix.ppm 1 mix
magick build/dev/eq-mix.ppm build/dev/eq-mix.png
```

PASS and visually checked: both captures are exactly 900×560 (≤1120×700), with
“Supple Audio” above the display name. The Linux preview embeds in a fixed-size
native parent, preventing the tiling WM from changing the capture dimensions.
X11 access for rendering required execution outside the sandbox.

REAPER installation/load/automation and non-Linux acceptance were not run;
independent review and STATUS.md remain with the orchestrator. No open questions.
