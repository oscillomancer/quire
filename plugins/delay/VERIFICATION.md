# Delay worker handoff

Branch `p3-delay` starts at `a9d667c`. The template instantiation is `c7cd2fb`;
implementation and final verification are subsequent commits on this branch.

## Changed files

- `plugins/delay/`: isolated template copy, SDK-free params/processor/tests,
  CLAP semantic/state/transport/tail integration, editor/theme/widget variant,
  preview scenarios, semantic/editor/tail host tests and CMake/docs.
- `plugins/CMakeLists.txt`, `tools/ci/format.sh`: delay registration/exclusion.
- `docs/specs/stock-effects.md` §6: algorithm choices, ranges, timing and
  freeze/tail limits, including the approved audio-thread tail correction.
- `skill/guides/delay.md`, `skill/SKILL.md`, `skill/guides/mixing.md`: every
  parameter, three recipes, honest limits and links.

No sibling/plugin-common/library/template/dependency source edits. The current
shared window, bridge, renderers, entry points, fonts, host contract, Linux
probe and Ring retain template identity after naming (16 infrastructure files
compared directly; font binaries exact, notices renamed). The generic CLAP
probe omits a meter lookup because delay uses GUI telemetry only. Product
Widgets adds centre-origin bipolar arcs. Obsolete gain DSP/CLI/example image
are removed. Independent review, STATUS and integration are orchestrator work.

## Verification

```sh
cmake --preset dev
cmake --build --preset dev -j6
build/dev/plugins/delay/engine/delay-engine.tests
ctest --test-dir build/dev/plugins/delay --output-on-failure
tools/validator/run.sh build/dev/plugins/clap/Echelon.clap
tools/ci/format.sh --check
git diff --check
python3 tools/license-check/check.py --root .
```

Dev build passed without first-party warnings. Engine: 12 cases / 3673
assertions passed. Plugin: 10/10 tests passed, including physical patch/state
conformance, concurrent patch replacement, gestures, mono/stereo and
float/double/in-place host contracts, transport/tail notifications and exports.
Validator 0.4.1: 36 success / 8 skipped. Format/diff and the license gate (12
existing dependencies) passed. The first validator run rejected 1234.57 Hz;
causal low-rate support and edge-rate tests were added before the passing run.

Measured DSP checks: fractional echo cross-correlation within one sample at
22.05–384 kHz (relative to the same filtered reference); 1 kHz repeat component
within 0.2 dB of feedback; loop magnitude within 0.05 dB at five sample rates;
bit-exact host partitions 1/7/64/4096 with automation; 375→200 ms glide without
clicks; ping-pong spacing; mono/width; 30 s bounded and sustained feedback 110;
20 s freeze RMS within 0.2 dB with new input rejected; 12 dB duck within 0.5 dB
and recovery; five seconds of full-domain automation; canonical restored
state/deterministic wow; causal validator rates through 768 kHz.

Real X11 commands ran outside the sandbox, without an audio device:

```sh
build/dev/plugins/delay/delay-gui-preview 20 build/dev/delay-default.ppm 1 default
build/dev/plugins/delay/delay-gui-preview 20 build/dev/delay-pingpong.ppm 1 pingpong
magick build/dev/delay-default.ppm build/dev/delay-default.png
magick build/dev/delay-pingpong.ppm build/dev/delay-pingpong.png
build/dev/plugins/delay/delay-linux-host-probe build/dev/plugins/clap/Echelon.clap build/dev/delay-host.ppm
magick build/dev/delay-host.ppm build/dev/delay-host.png
```

Both 900×560 previews were inspected: values below labels, clear of pointers,
repeat pattern, filters and units readable. The X11 host probe reported zero
failures across embedding, map, resize, hide/show, three reopen cycles,
timer cleanup and concurrent synthetic audio.

Full matrix command: `tools/ci/run-all.sh` once, outside the sandbox for socket
tests. Result pending at implementation commit; updated after completion.
The parent external-plugin helper forwards TSan/RTSan to child plugins;
ASan/UBSan cover parent engine/shared-library tests, while that helper does
not forward these flags to the child build. This is existing infrastructure.

## Remaining acceptance

REAPER load/automation and optional VST3/macOS/Windows acceptance were not run:
no installation/device access requested; this is the Linux CLAP checkout.
Optional VST3 self-handle/unload tests need its format build. No unresolved
implementation questions. Freeze period quantization, colored repeat
measurement and the tail estimate's limits are recorded in the spec/guide.
