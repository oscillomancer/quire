# Analyzer worker handoff

Branch: `p3-analyzer`. The canceled Margin work was removed before the clean
fast-forward to `integrate` and branch rename. No Margin changes are included.

## Changes

- `plugins/analyzer/`: copied template, SDK-free analyzer engine and tests,
  CLAP wrapper, editor, preview scenarios, host/semantic/editor tests and
  integration documentation. Display name is defined once as `Gauge`.
- `libs/core-analysis/`: new consumer-thread `LiveMeter`, rolling horizons
  in `LoudnessMeter`, their public headers, CMake registration and tests.
- `plugins/common/`: optional appended semantic telemetry callback, shared
  helper implementation, null callback for existing plugins and tests.
- `host/clap-host/src/clap_host.cpp` and
  `host/clap-host/tests/fixture_plugins.cpp`: accept the original semantic
  extension size and exercise that compatibility with the existing fixture.
- `plugins/CMakeLists.txt`, `tools/ci/format.sh`: analyzer registration and
  template infrastructure exclusions; only analyzer lines changed.
- `docs/specs/{stock-effects,core-analysis,semantic-params-extension}.md`:
  implementation decisions and interface documentation.
- `skill/guides/analyzer.md`, `skill/SKILL.md`, `skill/guides/mixing.md`:
  parameter reference, recipes, limits and guide links.

The engine passes audio bit-exactly at zero latency. It hands measurement
copies to a bounded queue and joined worker; hold, reset, window changes and
overflow never alter audio. The editor displays loudness, spectrum, history
and stereo statistics. Scalars are readonly params; semantic telemetry is
canonical JSON with the measured analysis-report fields.

## Verification run

All commands below ran in this worktree. No audio device was opened.

```sh
cmake --preset dev
cmake --build --preset dev -j6
build/dev/plugins/analyzer/engine/analyzer-engine.tests
ctest --test-dir build/dev/plugins/analyzer --output-on-failure
ctest --test-dir build/dev -j6 --output-on-failure -R 'analyzer|plugin-support|clap-host|core-analysis'
git diff --check
tools/ci/format.sh --check
python3 tools/license-check/check.py --root .
tools/ci/run-all.sh
```

- Dev configure/build passed; first-party targets were warnings-clean.
- Engine: 7 test cases, 8,178 assertions passed. Tests cover bit-exact output
  across rates, precision, channels and block sizes; automation/partition
  invariance; hold/reset/window/drop behavior; state and telemetry fields.
- Plugin subtree: all 8 tests passed, including host, semantic RT and editor
  input tests. The targeted monorepo selection passed all 7 tests, including
  the CLAP validator, host compatibility and offline/live analysis comparison.
- License gate passed with 12 dependencies. Format and diff checks passed.
- Full CI ran once outside the socket-restricted sandbox: license, format,
  dev, gcc, asan, ubsan, tsan and rtsan all passed. An earlier full dev CTest
  run inside the sandbox passed 40/45; the five failures were forbidden Unix
  sockets in control-protocol/MCP tests. The unrestricted full CI passed them.
- The shared external-plugin helper forwards TSan and RTSan instrumentation
  to the plugin child build. ASan/UBSan cover the parent engine/shared-library
  tests; that helper does not forward those flags to child plugin builds.

Real X11 editor checks and previews ran outside the sandbox to permit X11:

```sh
build/dev/plugins/analyzer/analyzer-gui-preview 20 build/dev/analyzer-idle.ppm 1 idle
build/dev/plugins/analyzer/analyzer-gui-preview 20 build/dev/analyzer-music.ppm 1 music
magick build/dev/analyzer-idle.ppm build/dev/analyzer-idle.png
magick build/dev/analyzer-music.ppm build/dev/analyzer-music.png
build/dev/plugins/analyzer/analyzer-linux-host-probe build/dev/plugins/clap/Analyzer.clap build/dev/analyzer-host.ppm
magick build/dev/analyzer-host.ppm build/dev/analyzer-host.png
```

Both 820×540 preview PNGs were inspected. The host probe passed with zero
failures: native parent embedding, mapping, nonblank drawing, resize,
hide/show, three reopen cycles, timer teardown and concurrent synthetic audio.
Artifacts remain under `build/dev/`; `Analyzer.clap` is under
`build/dev/plugins/clap/`. Sanitizer/GCC build trees were removed after CI.

The final follow-up restores one whitespace-only Ring line to the template
and exempts that shared header from formatting; no executable behavior
changed after the full CI. Format, license, diff and template identity checks
were repeated for that follow-up.

## Decisions and remaining acceptance

The analyzer section of `stock-effects.md` records window keys/default,
readonly scalar ranges, finite silence floors, hold/reset semantics, 100 ms
rolling observations, complete-block gating, core true-peak policy, the
smoothed mid spectrum and queue overflow behavior. The guide explains that
the spectrum cancels anti-phase content and that final delivery checks should
use offline analysis.

No new dependencies, command-bus commands or upstream template edits were
needed. There are no unresolved implementation questions. REAPER load and
automation acceptance, optional VST3, and macOS/Windows format acceptance
were not run: no installation/audio-device use was requested, and this is
the Linux CLAP worktree. Independent review, STATUS updates and integration
belong to the orchestrator; onboarding forbids worker-spawned subagents.
