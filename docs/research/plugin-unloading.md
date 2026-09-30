# Linux stock-plugin exports and unloading

The template's `3d32b8d` fix is carried into analyzer, comp, delay, drive, eq,
gate, orrery, probe-synth, transient and utility. Reverb is owned by its worker.
The five template files in each copy are byte-identical to that commit. Plugin
CMake validates all three pre-applied vendored wrapper patches without modifying
the dependency during configuration. `third_party/manifest.json` and
`THIRD_PARTY.md` record the new self-handle patch.

The Linux link maps expose only `clap_entry` for CLAP and exactly
`GetPluginFactory`, `ModuleEntry`, `ModuleExit`, `clap_entry` for VST3. The last
VST3 export is needed for the wrapper's lookup of its own statically linked
CLAP. `LINK_DEPENDS` makes map edits relink the modules. macOS and Windows
linking is unchanged. The engine's `orrery-headless.clap` uses the CLAP map too.

## Reproduction before the fix

Measured in this worktree on main `e2f9076` before applying the fix. The dev
preset uses Clang Debug; all ten stock modules and the in-tree headless module
were built before these measurements. The original unload probe was compiled
independently before the export lists or wrapper changes were applied:

```sh
cmake --preset dev
cmake --build --preset dev -j6
mkdir -p build/exports-evidence
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Wno-missing-field-initializers \
  -I third_party/clap/include plugins/common/tools/clap_unload_probe.cpp \
  -ldl -o build/exports-evidence/clap-unload-probe
nm -D --defined-only build/dev/plugins/clap/Plumb.clap
build/exports-evidence/clap-unload-probe "$PWD/build/dev/plugins/clap/Plumb.clap"
```

| Module (Clang dev) | Exports before | Exports after | Unload before | Unload after |
|---|---:|---:|---|---|
| Analyzer.clap (Gauge) | 3666 | 1 | pass | pass |
| Ballast.clap | 3427 | 1 | pass | pass |
| Echelon.clap | 1 | 1 | pass | pass |
| Facet.clap | 3885 | 1 | pass | pass |
| Flint.clap | 3401 | 1 | pass | pass |
| Gate.clap (Sluice) | 3469 | 1 | pass | pass |
| Kiln.clap | 3463 | 1 | pass | pass |
| Orrery.clap | 5435 | 1 | pass | pass |
| Plumb.clap | 3420 | 1 | pass | pass |
| ProbeSynth.clap | 3310 | 1 | pass | pass |
| orrery-headless.clap | 4603 | 1 | pass | pass |

Clang emitted no GNU-unique dynamic symbols in these modules, so its dev
unload successes alone would not reproduce the pinning bug. A separate,
unmodified GCC Debug Plumb build with GUI/tools off did reproduce it:

```sh
cmake -S plugins/utility -B build/exports-before-gcc -G Ninja \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug \
  -DUTILITY_BUILD_GUI=OFF -DUTILITY_BUILD_TOOLS=OFF
cmake --build build/exports-before-gcc -j6
nm -D --defined-only build/clap/Plumb.clap
build/exports-evidence/clap-unload-probe "$PWD/build/clap/Plumb.clap"
```

That module exported **4783 symbols, including 23 GNU-unique symbols**. Its
plugin initialized, activated and processed eight silence blocks, but the probe
exited 1 with `pinned: module is still mapped after dlclose`. This distinguishes
an actual loader pin from a failure to load or run the DSP. Export counts vary
by compiler, GUI build and optimization; the post-fix requirement is exact.
The matching GCC rebuild after the fix exports one symbol, has no GNU-unique
exports and passes both unload and reload/exit cycles. Local evidence is retained
in `build/exports-evidence/{before,after}.json`.

## Probe and regression checks

`plugins/common/tools/clap_unload_probe.cpp` is original standard-library/CLAP
host code. For every factory descriptor it creates and initializes the plugin,
allocates port buffers while inactive, activates at 48 kHz with a 1..512 frame
range, starts processing, processes eight 256-frame silence blocks on the
calling thread, checks finite output, stops, deactivates and destroys it.
Inputs include all declared ports, including sidechains. Host extensions are
null and requests are no-ops; no editor or audio device is opened.

The first cycle calls entry deinit and `dlclose`, then requires a
`RTLD_NOLOAD` lookup to fail. A successful lookup means pinned and is released
before reporting failure. The second cycle runs the same lifecycle and calls
entry deinit, but deliberately leaves the image mapped for process exit.
Gauge's consumer thread is already stopped and joined on deactivate/destroy;
no change to its DSP or lifecycle was needed.

The parent build registers `plugin.<dir>.unload` for all ten copies and
`plugin.orrery-headless.unload`, labelled `rt;plugin-unload` with a 30-second
process timeout. No sanitizer preset is excluded. Each copy runs its template
`exports-clap` check whenever tools are enabled; the headless module has
`orrery-clap.exports`. VST3 builds additionally run the template's
`exports-vst3`, `vst3-unload` and `wrapper-self-handle` checks. The VST3 probe
unloads twice, then exits with the third load mapped.

```sh
ctest --test-dir build/dev \
  -R '(^plugin\..*\.unload$|^orrery-clap.exports$|^plugin\..*\.ctest$)' \
  --output-on-failure -j6
```

The focused dev run passed all 22 parent checks (including the ten nested
plugin suites); all eleven measured modules export exactly `clap_entry` and
pass the standalone unload probe after the fix.

## Opt-in VST3 verification

The vendored VST3 hosting subset lacks the complete plugin SDK's
`public.sdk/source/main/pluginfactory.cpp`. The parent forwards an optional
`VST3_SDK_ROOT` cache path to each isolated plugin build. Verification uses a
temporary copy of the template's existing MIT SDK cache under this worktree's
`build/exports-vst3-sdk`, without modifying the template or downloading code:

```sh
cmake -S . -B build/exports-vst3 -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DSUPPLE_BUILD_GUI=OFF -DSUPPLE_PLUGIN_VST3=ON \
  -DVST3_SDK_ROOT="$PWD/build/exports-vst3-sdk"
cmake --build build/exports-vst3 -j6
for plugin in analyzer comp delay drive eq gate orrery probe-synth transient utility; do
  ctest --test-dir "build/exports-vst3/plugins/$plugin" \
    -R '^(exports-vst3|vst3-unload|wrapper-self-handle)$' --output-on-failure
done
```

All **30 checks passed**: exports, unload/reload/exit and wrapper patch presence
for each of the ten VST3 plugins. Each VST3 exports exactly the four allowed
entry points. The temporary SDK and VST3 build are removed after verification; logs remain
under `build/exports-vst3-*.log`. Full preset verification runs
`tools/ci/run-all.sh` once after the focused checks.

## Other observations

- The first baseline build failed while staging Gate's `NOTICES.txt` into the
  shared CLAP output directory. Both paths existed; an unchanged build retry
  completed. This was separate from the loader/probe measurements.
- The byte-identical self-handle patch ends in a space-only unified-diff hunk
  context line. Git's whitespace check flags that inherited patch text. The
  non-patch changes pass `git diff --check`; patch bytes are preserved and
  reverse-application checks pass.

## Final CI verification

`SUPPLE_AUDIO_DEVICE_TEST=0 tools/ci/run-all.sh` was run once outside the
socket-restricted sandbox. License and format checks passed. Dev, GCC, ASan,
UBSan and RTSan passed. TSan passed 63/64 checks: its only failure was
`engine.tests`, reporting a double mutex lock in the system `libspa-support`
through PipeWire in the real JACK test. The intentionally crashing bridge
fixture also emits sanitizer crash diagnostics in that test's log.

All **66 unload checks passed** (eleven modules in each of the six presets),
including TSan. All nested stock-plugin suites, including their export checks,
passed in every preset. No unload test is skipped for a sanitizer.

Verification mistake: the real JACK opt-in checks whether
`SUPPLE_AUDIO_DEVICE_TEST` exists, rather than whether it equals `1`. Setting
it to `0` therefore unintentionally enabled the two-second real-device test.
The failed TSan engine test was re-run with the variable absent using:

```sh
env -u SUPPLE_AUDIO_DEVICE_TEST ctest --preset tsan --rerun-failed -j6 --output-on-failure
```

The focused re-run **passed (1/1), in 196.72 seconds**. The original CI result
and log are preserved; a focused re-run does not change
the original full-run exit status. The audio-device flag is not needed for any
export or unload check, and no engine-test implementation is changed here.
