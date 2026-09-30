# Gate worker handoff

Branch `p3-gate` was created from `integrate` in the existing worktree.

## Changed files

- `plugins/gate/`: instantiated template, engine params/processor/tests,
  CLAP integration, editor/theme/telemetry, preview, host/semantic/editor
  tests, CMake and integration documentation. Shared infrastructure remains
  identical after identity renames; no template or sibling plugin edits.
- `plugins/CMakeLists.txt`, `tools/ci/format.sh`: only gate registration and
  shared-infrastructure exclusion lines.
- `docs/specs/stock-effects.md`: only gate implementation decisions.
- `skill/guides/gate.md`, `skill/SKILL.md`, `skill/guides/mixing.md`: every
  key/unit/range, three recipes, limits and guide links.

The implementation includes gate/expander hysteresis and hold, dB one-pole
ballistics, filtered internal/external stereo keys, linking, aligned mix/key
listen, readonly meters and a deferred lookahead restart contract. The
lookahead opening ramp is normalized to reach unity before the delayed
transient without a terminal gain jump. Processor state and controls are
sample-counted, independent of host partitioning.

## Verification

Commands run from this worktree:

```sh
cmake --preset dev
cmake --build --preset dev -j6
build/dev/plugins/gate/engine/gate-engine.tests
ctest --test-dir build/dev/plugins/gate --output-on-failure
ctest --test-dir build/dev -j6 --output-on-failure
tools/validator/run.sh build/dev/plugins/clap/Gate.clap
git diff --check
tools/ci/format.sh --check
python3 tools/license-check/check.py --root .
```

Results: dev build passed, first-party warnings clean; engine 10 cases /
490,354 assertions passed; plugin CTest 8/8; monorepo dev CTest 51/51.
CLAP validator 0.4.1 passed 36 applicable checks (8 skipped). License gate
passed with 12 dependencies; format/diff checks passed. A direct identity
comparison passed for 14 copied infrastructure files and the Ring. The
first validator run rejected 8 kHz; rate support and edge-rate delay tests
were corrected before the passing runs.

Dev CTest ran outside the sandbox to permit Unix sockets. Real X11 checks
also ran outside it, using synthetic audio with no audio-device access:

```sh
build/dev/plugins/gate/gate-gui-preview 20 build/dev/gate-default.ppm 1 default
build/dev/plugins/gate/gate-gui-preview 20 build/dev/gate-drums.ppm 1 drums
magick build/dev/gate-default.ppm build/dev/gate-default.png
magick build/dev/gate-drums.ppm build/dev/gate-drums.png
build/dev/plugins/gate/gate-linux-host-probe build/dev/plugins/clap/Gate.clap build/dev/gate-host.ppm
magick build/dev/gate-host.ppm build/dev/gate-host.png
```

Both 820×560 previews were inspected. The X11 host probe passed with zero
failures: native parent embedding, mapping, nonblank rendering, resize,
hide/show, three reopen cycles, timer cleanup and concurrent synthetic audio.
PNG files and the CLAP artifact remain under `build/dev/`.

Full matrix command: `tools/ci/run-all.sh` (once, outside the sandbox).
Result: pending at implementation commit; updated after completion.
The shared external-plugin helper forwards TSan/RTSan to the child build;
ASan/UBSan instrument parent engine/shared-library tests, while that helper
does not forward those flags to child plugin builds.

## Remaining acceptance

The gate spec section records default/range choices, expander deadband
behavior, exact mute settling, ballistics timing, normalized lookahead
opening, active latency invariance, smoothing and meter definitions. No
shared DSP edits, new dependencies or command-bus commands were needed.

REAPER load/automation and optional VST3/macOS/Windows format acceptance
were not run: no installation or audio-device use was requested, and this
is the Linux CLAP worktree. Independent review, STATUS update and integration
belong to the orchestrator; onboarding forbids worker-spawned subagents.
There are no unresolved implementation questions.
