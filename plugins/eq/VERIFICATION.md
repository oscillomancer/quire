# EQ worker verification — 2026-09-29

Branch: `p3-eq`. Linux workstation, dummy/fake audio backends; no installation
or real audio-device access. The display identity is in `identity.cmake`.

## Build and full matrix

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --test-dir build/dev -j6 --output-on-failure
tools/ci/run-all.sh
```

The initial dev build passed. One unrestricted full-CI attempt was externally
terminated (SIGTERM, exit 143); its incomplete log is
`build/ci-dev-unrestricted-interrupted.log`. The subsequent complete matrix
ran in the sandbox. License and format passed; EQ passed in every preset.
The complete matrix returned 1 because the socket tests cannot open sockets
in this sandbox (`Operation not permitted`):

| Preset | Whole-repository result | EQ result |
|---|---|---|
| dev | 40/45 pass | engine, plugin, validator pass |
| gcc | 40/45 pass | engine, plugin, validator pass |
| asan | 36/40 pass | engine and plugin pass |
| ubsan | 35/39 pass | engine and plugin pass |
| tsan | 34/38 pass | engine and plugin pass |
| rtsan | 36/40 pass | engine and plugin pass |

Failures: `control-protocol.tests`, `sandbox.mcp-proxy`, `engine.mcp-proxy`,
`mcp.tests`, plus `mcp.demo` in dev/GCC. These are the onboarding-documented
sandbox limitations; no socket test was changed. Logs: `build/ci-<preset>.log`.

## Final corrections

After the complete matrix, review found unbounded auto-gain compensation for
extreme stopbands, resampling choices affected by ignored filter controls,
and cancellation of side-only audio in spectrum telemetry. Those were corrected;
auto gain now clamps to ±24 dB, ignored controls have bit-identical regression
coverage, and LTAS analyzes separate stereo channels. The GUI probe measures
actual output sample peaks rather than reading a nonexistent meter parameter.

```sh
for p in dev gcc asan ubsan tsan rtsan; do
  cmake --build --preset "$p" --target eq-engine.tests plugin-eq -j6
  ctest --preset "$p" \
    -R '(^eq-engine.tests$|^plugin.eq.ctest$|^plugin.eq.validator$)' \
    --output-on-failure
done
```

PASS: dev and GCC 3/3 each; ASan, UBSan, TSan and RTSan 2/2 each (validator
is intentionally excluded by those presets). Aggregate plugin CTest includes
host-smoke stereo/mono, host contracts, semantic/state/RT checks, editor input,
and wrapper patch checks. Logs: `build/eq-<preset>-final-{build,tests}.log`.

The five-second full-range automation stress fixture was subsequently extended
to all three phase modes and two seconds of silent tails. Phase stays fixed per
activation, with restart behavior separately checked by the host regression.
Linear kernel publication coalesces on a deterministic 4096-sample grid while
all automatable targets jump every 1–64 samples. The fixture uses −480 dBFS
noise to exercise cumulative boosts without a hidden ceiling limiter.

```sh
for p in dev gcc asan ubsan tsan rtsan; do
  cmake --build --preset "$p" --target eq-engine.tests -j6
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  TSAN_OPTIONS=halt_on_error=1 RTSAN_OPTIONS=halt_on_error=1 \
    "build/$p/plugins/eq/engine/eq-engine.tests" \
    -tc='EQ full-range automation*'
done
```

PASS in all six presets. Logs: `build/eq-<preset>-stress-{build,tests}.log`.

```sh
python3 tools/license-check/check.py --root .
tools/ci/format.sh --check
git diff --cached --check -- . ':!plugins/eq/cmake/*.patch'
```

PASS: 12 approved dependencies, formatting and staged whitespace. The two
verbatim template patch files retain required blank context-line spaces and
are excluded from the whitespace check. No new dependency
or shared library changes. Local half-band and FFT/convolution primitives are
documented in `QUIRE.md`.

## Previews and remaining acceptance

```sh
build/dev/plugins/eq/eq-gui-preview 0 build/dev/eq-flat.ppm 1 flat
magick build/dev/eq-flat.ppm build/dev/eq-flat.png
build/dev/plugins/eq/eq-gui-preview 0 build/dev/eq-mix.ppm 1 mix
magick build/dev/eq-mix.ppm build/dev/eq-mix.png
```

PASS and visually checked: `build/dev/eq-flat.png`, `build/dev/eq-mix.png`,
1259×683 workstation captures of the 900×560 design. Rendering required access
to the workstation X11 display outside the sandbox.

REAPER installation/load/automation, real-device and macOS/Windows acceptance
were not run. Independent review and `STATUS.md` updates belong to the
orchestrator. No open worker questions; the user approved `db6`…`db96` slope
keys. All implementation decisions are recorded in stock-effects.md §1.
