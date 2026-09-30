# Reverb verification — 2026-09-30

Branch p3-reverb, base a9d667c; no rebase. All commands ran in this worktree.

## Test-cost review follow-up

No production DSP changes. Removed the TSan TIMEOUT override. The full
matrix remains in `reverb-engine.measure`, an EXCLUDE_FROM_ALL target not
registered with CTest, compiled from the same source with
REVERB_MEASURE_SWEEPS. Routine tests use representative short IRs and
aggregate sample predicates without dropping any samples. The ten-second
freeze comparison and five-second automation stress remain in CTest.

| Final routine checks | Engine runtime | Product suite runtime | Result |
|---|---:|---:|---|
| dev | 4.28 s | 1.23 s | both passed |
| TSan | 16.44 s | 5.30 s | both passed; no race report |

Both engine runs passed 9 cases / 184 assertions. The representative RT60
subset's worst error is 8.0412% (below 10%; its flat-low-shelf/damping-2-kHz
shape differs from the dense matrix). Short-IR density crossings remain
67/52 ms. The short modulated-decay check measured 1.001824 s against 1 s.
These elapsed times are CTest measurements, not pass/fail assertions.

Exact verification:

```sh
cmake --preset dev
cmake --build --preset dev --target reverb-engine.tests reverb-engine.measure plugin-reverb -j6
ctest --preset dev -R '^(reverb-engine.tests|plugin.reverb.ctest)$' --output-on-failure
cmake --build --preset dev -j6
cmake --preset tsan
cmake --build --preset tsan --target reverb-engine.tests plugin-reverb semantic-conformance -j6
ctest --preset tsan -R '^(reverb-engine.tests|plugin.reverb.ctest)$' --output-on-failure
cmake --preset release -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG'
cmake --build --preset release --target reverb-engine.measure -j2
build/release/plugins/reverb/engine/reverb-engine.measure
```

The opt-in runner passed 9 cases / 582 aggregated assertions, preserving
all 90 responses, seven octave bands, original density/stereo windows and
the 44.1/48 kHz long modulated tails. Dense-matrix RT60/density results below
were reproduced unchanged. Generated dev/TSan CTest files register only
reverb-engine.tests and contain no TIMEOUT property. Format and license
checks pass. No full CI rerun, per the review instruction. The temporary
TSan tree was removed afterwards; dev, release and verification logs remain.

The following records describe initial bce7a68 verification. The historical
1800-second workaround and million-assertion suite are superseded by this
cost fix; the DSP measurements remain valid and were rerun above.

## Measurements

RT60 uses octave-filtered stereo impulse energies, Schroeder integration and
T30 regression (-5..-35 dB), not timing assertions. The 90-response matrix
covers 44.1/48/88.2/96/192 kHz, decay 0.5/2/8 s, size 20/50/90 and two
shapes: defaults; and low_decay 1.5, crossover 1 kHz, damping 2 kHz.
High_decay 0.5, modulation 0, early off, mix 100. Only octave bands at least
an octave from both transitions are compared against plateau targets.

| Band | Worst absolute relative RT60 error |
|---:|---:|
| 125 Hz | 7.4396% |
| 250 Hz | 4.3463% |
| 500 Hz | 7.4759% |
| 1 kHz | 6.0268% |
| 2 kHz | 7.6802% |
| 4 kHz | 6.3230% |
| 8 kHz | 2.7007% |

Separate default-modulation coverage, size 20 / decay 8 s / damping 2 kHz:
8 kHz RT60 is 3.945919 s at 44.1 kHz and 3.949229 s at 48 kHz, against
4 s (1.35% / 1.27% error). The approved second-order Butterworth-Q shelves,
diffuser-tail cap and twelve-point FDN interpolation are documented in
stock-effects.md §7. Extreme ratios and overlapping transitions remain
outside this measurement matrix.

Normalized echo density reaches 0.8 at 67 ms by default, 52 ms with diffusion
100 (20 ms window, sustained crossing for 10 ms). Late-tail correlation is
0.049305 at width 100; width 0 renders bit-identical channels. Freeze's
ten-second stereo tail energy comparison changes -0.0326 dB.

CPU benchmark: Clang -O2 -DNDEBUG, 48 kHz stereo, 512-frame blocks, editor
closed/telemetry disabled, median of five CPU-time trials, 120 audio seconds
per trial. Default **1.5126%**, hall **1.6604%**, maximum texture **1.8417%**
of one core; all below 2%. Maximum texture uses size 100, decay 60 s,
low_decay 4, high_decay 0.1, diffusion/modulation 100, width 200, mix 100.

## Commands and results

- `cmake --preset dev && cmake --build --preset dev -j6`: built successfully.
- `build/dev/plugins/reverb/engine/reverb-engine.tests`: 9 cases,
  1,085,971 assertions passed (final implementation).
- `ctest --test-dir build/dev/plugins/reverb --output-on-failure -j5`:
  all 10 product tests passed, including concurrent semantic edits and
  audio-thread tail notifications.
- `ctest --test-dir build/dev -R 'plugin.reverb.validator' --output-on-failure`:
  pinned CLAP validator passed.
- `ctest --test-dir build/dev -j6 --output-on-failure`: baseline socket
  restrictions fail control-protocol.tests, sandbox.mcp-proxy, mcp.demo,
  engine.mcp-proxy and mcp.tests; other tests passed.
- `cmake --preset release -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG'`;
  `cmake --build --preset release --target reverb-bench -j6`;
  `build/release/plugins/reverb/engine/reverb-bench 120`: figures above.
- `build/dev/plugins/reverb/reverb-gui-preview 0 build/dev/reverb-default.ppm 1 default`
  and the same command with `reverb-hall.ppm` / `hall`: rendered successfully.
  `magick build/dev/reverb-default.ppm build/dev/reverb-default.png` and the
  corresponding hall conversion; both PNGs inspected, 900 × 560.
- `build/dev/plugins/reverb/reverb-linux-host-probe build/dev/plugins/clap/Nave.clap build/dev/reverb-host.ppm`:
  zero failures; actual X11 embedding, resize, show/hide, three reopen cycles,
  timer teardown and concurrent in-memory audio. No audio device opened.
- `python3 tools/license-check/check.py --root .`: 12 dependencies, passed.
- `tools/ci/format.sh --check`: passed.
- `build/dev/plugins/reverb/reverb-cli build/dev/reverb-cli-input.wav build/dev/reverb-cli-output.wav mix '100%' decay '2 s' predelay '25 ms' early '-inf' low_decay '1.5 x' high_decay '0.4 x'`:
  rendered a one-second stereo impulse WAV; independent Python inspection
  found 48,000 finite stereo frames, an audible wet tail and exactly silent
  first 25 ms. Input was generated with Python wave/struct, no device.
- `tools/ci/run-all.sh`: run once; license and format passed. All builds
  completed. Repository test outcomes:

  | Preset | Passed / total | Failures |
  |---|---:|---|
  | dev | 52 / 57 | five socket tests |
  | gcc | 52 / 57 | five socket tests |
  | asan | 44 / 48 | four socket tests |
  | ubsan | 43 / 47 | four socket tests |
  | tsan | 41 / 46 | four socket tests; reverb measurement timeout at 600 s |
  | rtsan | 44 / 48 | four socket tests |

  Socket failures are control-protocol.tests, sandbox.mcp-proxy,
  engine.mcp-proxy and mcp.tests; dev/gcc also fail mcp.demo. Logs identify
  `rpc.socket: Operation not permitted`; no socket code was changed.
  The reverb engine and product tests passed in every other preset; TSan's
  product suite passed too, with no race report. The engine's full numerical
  matrix now has a TSan-only 1800-second harness allowance, preserving all
  coverage and numerical assertions. Its targeted rerun result follows below.

- After the late unit/export fixes, rebuilt `plugin-reverb` and
  `reverb-engine.tests` in all presets (at most -j6; other targeted rebuilds use -j5 alongside
  the single TSan test). Re-ran the final full engine suite in dev and rtsan, the
  unit/patch case in gcc/asan/ubsan, and each product suite including exports.
  ASan CTest uses `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`, matching the
  preset; an initial manual rerun without it hit the sandbox's LeakSanitizer
  ptrace restriction, then passed with the preset setting. Also re-ran the dev
  validator, previews and X11 lifecycle probe on the restricted-export module.
  `nm -D --defined-only build/dev/plugins/clap/Nave.clap` lists only clap_entry.

- `cmake --preset tsan && cmake --build --preset tsan --target plugin-reverb reverb-engine.tests -j6`;
  `ctest --preset tsan -R '^(reverb-engine.tests|plugin.reverb.ctest)$' --output-on-failure`:
  **both passed**; full engine suite 721.63 s, product suite 11.71 s.
  No TSan race report. The 600 s timeout is resolved by the harness allowance;
  the full matrix and numerical thresholds remain intact.
- Final metadata/export rechecks for `p` in gcc/asan/ubsan/rtsan:
  `cmake --preset "$p" && cmake --build --preset "$p" --target plugin-reverb reverb-engine.tests -j5`;
  `build/$p/plugins/reverb/engine/reverb-engine.tests -tc='Reverb table*'`
  (rtsan ran the entire executable instead); and
  `ctest --test-dir build/$p/plugins/reverb --output-on-failure -j5` with the
  preset's sanitizer environment. All passed. Dev/GCC product suites have
  10 tests; GUI-disabled sanitizer product suites have 9.
- Deleted build/gcc, build/asan, build/ubsan, build/tsan and build/rtsan
  after verification; retained build/dev, release benchmark and CI logs.

Identity grep: `rg -n '"Nave"' plugins/reverb` returns only
`cmake/Identity.cmake`; C++ gets SUPPLE_REVERB_DISPLAY_NAME and the artifact
is `build/dev/plugins/clap/Nave.clap`.

## Scope and remaining acceptance

No new dependencies or shared-library/template edits; no changes to STATUS.md.
No hardware/REAPER acceptance or macOS/Windows testing on this Linux worker.
The orchestrator owns cross-platform acceptance, independent review and merge.
The Linux export maps and exports-clap check are wired for CLAP/VST3 targets.
`git apply --reverse --check plugins/reverb/cmake/clap-wrapper-vst3-self-handle.patch --directory=third_party/clap-wrapper`
fails at fsutil.cpp:378 on this base. Per the orchestrator's instruction,
the fatal presence gate is left out until the shared vendor patch is merged;
no vendored source was changed. VST3 itself was not built here.

No open DSP questions. GUI previews are drawing checks; the X11
host probe separately checks embedding/lifecycle behavior.
