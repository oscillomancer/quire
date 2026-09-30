# Console 9 worker verification

Branch `p1-console9`, base `8fdb7bb`. Port `82d6c2e` reviewed and approved by
the orchestrator; the linear addition awaits review. Integration and STATUS
changes are orchestrator work. No `host/` changes.

## Null acceptance

Release and dev null matrices pass. Each row compares 2,580,480 samples from
315 native fader/pan/rate combinations. Input includes sine sweeps/noise at
−60…+12 dBFS, silence and both sides of the float/double 1.18e−23 threshold.
Block 1 is bit-identical throughout. At every block size all original
block-endpoint samples are identical. Supplying the original per-block
interpolated coefficients to the port reproduces **every** reference sample
bit-for-bit; the unexplained difference count is zero in all rows.

| Stage | Bits | Block | Identical % | Max float ulps, all | Max absolute error | Ordinary max float ulps |
|---|---:|---:|---:|---:|---:|---:|
| channel | 32 | 1 | 100.000000 | 0 | 0 | 0 |
| channel | 32 | 7 | 94.559152 | 3.87688e+37 | 5.44679e-08 | 0 |
| channel | 32 | 64 | 99.702381 | 2.85626e+25 | 5.44546e-08 | 0 |
| channel | 32 | 512 | 98.511905 | 3.87842e+37 | 5.44639e-08 | 0 |
| channel | 32 | 4096 | 98.549107 | 3.8504e+37 | 5.44639e-08 | 0 |
| channel | 64 | 1 | 100.000000 | 0 | 0 | 0 |
| channel | 64 | 7 | 89.192437 | 1.67772e+07 | 2.6739e-15 | 1.49012e-08 |
| channel | 64 | 64 | 99.212123 | 1.67765e+07 | 2.26348e-15 | 1.67638e-08 |
| channel | 64 | 512 | 96.800014 | 1.67768e+07 | 2.41525e-15 | 1.30385e-08 |
| channel | 64 | 4096 | 96.888060 | 1.67772e+07 | 2.08449e-15 | 1.30385e-08 |
| buss | 32 | 1 | 100.000000 | 0 | 0 | 0 |
| buss | 32 | 7 | 94.559152 | 3.87688e+37 | 5.44679e-08 | 0 |
| buss | 32 | 64 | 99.702381 | 1.42813e+25 | 5.44546e-08 | 0 |
| buss | 32 | 512 | 98.511905 | 3.87842e+37 | 5.44639e-08 | 0 |
| buss | 32 | 4096 | 98.549107 | 3.8504e+37 | 5.44639e-08 | 0 |
| buss | 64 | 1 | 100.000000 | 0 | 0 | 0 |
| buss | 64 | 7 | 87.808625 | 1.67768e+07 | 1.77636e-15 | 9.31323e-09 |
| buss | 64 | 64 | 99.142485 | 1.67762e+07 | 1.77636e-15 | 1.11759e-08 |
| buss | 64 | 512 | 96.352694 | 1.67768e+07 | 1.77636e-15 | 1.11759e-08 |
| buss | 64 | 4096 | 96.483212 | 1.67768e+07 | 1.77636e-15 | 1.30385e-08 |

"Ordinary" excludes only the hard-right left output (A=1). At that edge,
upstream's `a*t+a*(1-t)` introduces a tiny ±pan gain instead of exact zero;
float dither sees a different exponent and the difference can reach ~5.45e−8.
Relative ulps against the near-zero reference value are consequently large.
Double errors elsewhere remain at rounding scale. `README.md` explains this
fully, including the test-only coefficient proof. These differences are from
the documented switch to partition-independent settings/ramps, not altered
curve or dither arithmetic. Mono is checked against the fader-only equivalent
stereo hard-left path; the retained AU text is not compiled.

Five encoded channels summed and decoded by a neutral buss:

| Bits | Block | Identical % | Max float ulps | Max absolute error |
|---|---:|---:|---:|---:|
| 32 | 1 | 100.000000 | 0 | 0 |
| 64 | 1 | 100.000000 | 0 | 0 |
| 32 | 7 | 100.000000 | 0 | 0 |
| 64 | 7 | 68.383789 | 1.6652e-06 | 1.11022e-16 |
| 32 | 64 | 100.000000 | 0 | 0 |
| 64 | 64 | 96.301270 | 8.32602e-07 | 5.55112e-17 |
| 32 | 512 | 100.000000 | 0 | 0 |
| 64 | 512 | 90.966797 | 1.06171e-07 | 8.32667e-17 |
| 32 | 4096 | 100.000000 | 0 | 0 |
| 64 | 4096 | 90.893555 | 4.17233e-07 | 5.55112e-17 |

All encoder and decoder differences in the sum test are also reproduced
exactly by the coefficient-injection diagnostic. Ramp split tests compare
sample bits, including signed zero, for blocks 1/7/64/512/4096 in all four
stage types and both precisions, including retargeting.

## Release benchmark (reviewed port)

Clang Release, workstation observation, 512-frame blocks ×4096 iterations
following 32 warm-up blocks. This worktree's CI/build was idle during timing.

| Stage | Bits | Settings | ns/stereo frame | One core at 48 kHz |
|---|---:|---|---:|---:|
| channel | 32 | constant | 37.009 | 0.1776% |
| channel | 32 | ramping | 41.275 | 0.1981% |
| buss | 32 | constant | 27.412 | 0.1316% |
| buss | 32 | ramping | 39.146 | 0.1879% |
| channel | 64 | constant | 15.972 | 0.0767% |
| channel | 64 | ramping | 26.720 | 0.1283% |
| buss | 64 | constant | 14.583 | 0.0700% |
| buss | 64 | ramping | 23.309 | 0.1119% |

## Commands and results

```sh
cmake --preset dev
cmake --build --preset dev -j6 --target console9.tests console9-bench
build/dev/libs/console9/console9.tests
cmake --preset release
cmake --build --preset release -j6 --target console9.tests console9-bench
build/release/libs/console9/console9.tests
build/release/libs/console9/console9-bench
sha256sum -c build/console9-upstream.sha256
python3 tools/license-check/check.py --root .
tools/ci/format.sh --check
git diff --cached --check -- . ':!third_party/airwindows-console9'
```

Dev/release targeted builds/tests passed; upstream's nine original files
remained byte-identical to the supplied copy, verified by recorded hashes.
License gate passed with 13 dependencies. Format and first-party diff checks
passed; upstream whitespace was retained byte-identically (the unrestricted
cached diff check reports those original trailing spaces).
Only test bridge accessors touch private fields; the stand-in is original,
non-ABI test code, without the proprietary SDK or other compatibility code.
An additional tests-disabled Release configure/build of `console9` passed
and its Ninja graph contains neither the reference target nor upstream files:

```sh
cmake -S . -B build/console9-no-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSUPPLE_BUILD_TESTS=OFF -DSUPPLE_BUILD_PLUGINS=OFF -DSUPPLE_BUILD_GUI=OFF
cmake --build build/console9-no-tests -j6 --target console9
```

Full matrix: `tools/ci/run-all.sh` once, outside the sandbox for sockets.
License, format and all six presets passed. No sample-rate API, device access,
engine integration or stock-plugin changes: this task supplies the DSP
library only. No open implementation questions.

## Level-matched linear addition

Nate's decision/amended ADR-0019 §6: `setLinear(true)` retains fader gain,
pan law and ramps, skipping only curves, denormal replacement and dither.
Channel also skips the ×0.618 pre-scale (its product with the encoder's
small-signal slope is unity); Buss uses the exact slope 0.6180339887498949
before pan and fader. Dispatch selects a separate loop once per block.
There is no RNG advance, noise, clipping or internal mode crossfade. A plan
rebuild controls the on/off transition. Nonfinite input is still treated as
zero. All four stages support the mode, and it defaults off (curves on).

Tests verify exact neutral cascade gain over signed powers-of-two from
2^−40 to 2^20, with no clipping; small signals versus curves within two float
ulps (dither) or 16 double ulps (roundoff; reference double additive dither is
disabled); bit-exact split ramps; silent zero output and actual tiny-input
preservation; nonfinite input; and unchanged RNG progress by matching the
next curves-on zero-input sample against a control after a prior common
sample and 1000 linear samples. All checks cover stereo/mono and both I/O
precisions. The full curves-on null matrix still has zero unexplained
samples; the table above is updated to the final Release run.

Commands for the addition:

```sh
cmake --build --preset dev -j6 --target console9.tests console9-bench
build/dev/libs/console9/console9.tests -tc='Console9 linear*'
build/dev/libs/console9/console9.tests
cmake --build --preset release -j6 --target console9.tests console9-bench
build/release/libs/console9/console9.tests
build/release/libs/console9/console9-bench
cmake --build --preset gcc -j6 --target console9.tests
ctest --test-dir build/gcc -R '^console9\.tests$' --output-on-failure
for preset in asan ubsan tsan rtsan; do
  cmake --build --preset "$preset" -j6 --target console9.tests
  ctest --preset "$preset" -R '^console9\.tests$' --output-on-failure
done
tools/ci/format.sh --check
git diff --check
sha256sum -c build/console9-upstream.sha256
```

Dev/Release: 12 cases / 70431 assertions pass. Four new cases / 19956
assertions pass. GCC/ASan/UBSan/TSan/RTSan focused builds and complete library
tests pass; format/diff checks and upstream hashes pass. The first direct
ASan CTest invocation passed all assertions, then LeakSanitizer could not run
under the sandbox's ptrace; using the repository test preset (detect_leaks=0,
as in full CI) passed. The reviewed port's full CI passed all six presets;
the addition reran focused library checks in all presets rather than another
full matrix. No engine code was changed.

Updated Clang Release benchmark, same setup, no build/CI in this worktree
running during timing:

| Stage | Bits | Mode | Settings | ns/stereo frame | One core at 48 kHz |
|---|---:|---|---|---:|---:|
| channel | 32 | curves | constant | 32.657 | 0.1568% |
| channel | 32 | curves | ramping | 37.458 | 0.1798% |
| buss | 32 | curves | constant | 25.417 | 0.1220% |
| buss | 32 | curves | ramping | 35.349 | 0.1697% |
| channel | 64 | curves | constant | 14.105 | 0.0677% |
| channel | 64 | curves | ramping | 23.710 | 0.1138% |
| buss | 64 | curves | constant | 13.157 | 0.0632% |
| buss | 64 | curves | ramping | 20.727 | 0.0995% |
| channel | 32 | linear | constant | 1.465 | 0.0070% |
| channel | 32 | linear | ramping | 7.076 | 0.0340% |
| buss | 32 | linear | constant | 1.525 | 0.0073% |
| buss | 32 | linear | ramping | 7.332 | 0.0352% |
| channel | 64 | linear | constant | 0.652 | 0.0031% |
| channel | 64 | linear | ramping | 6.783 | 0.0326% |
| buss | 64 | linear | constant | 0.945 | 0.0045% |
| buss | 64 | linear | ramping | 6.905 | 0.0331% |

Cleanup: `rm -rf -- build/gcc build/asan build/ubsan build/tsan build/rtsan`
completed after verification. Dev/Release, benchmark logs and hash evidence
are retained. No processes or audio device left running. No open
implementation questions; follow-up review/integration remain orchestrator
work.
