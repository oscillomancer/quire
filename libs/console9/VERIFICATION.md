# Console 9 worker verification

Branch `p1-console9`, base `8fdb7bb`. Reviewed and approved by the orchestrator. Integration and
STATUS changes are orchestrator work. No `host/` changes.

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

## Release benchmark

Clang Release, workstation observation, 512-frame blocks ×4096 iterations
following 32 warm-up blocks. No full CI/build ran concurrently with timing.

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
