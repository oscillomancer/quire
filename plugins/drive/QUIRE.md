# Quire integration notes (Kiln)

A direct copy of `~/Projects/audio-plugins/template`, renamed
Starter/starter/STARTER → Drive/drive/DRIVE. Identity: `com.suppleaudio.drive`,
AU `QDrv`/`Supl`/`aufx`, state `QDR1`, version 0.1.0. The working display name
is defined once in `cmake/Identity.cmake`, passed to the engine as
`SUPPLE_DRIVE_DISPLAY_NAME`.
The copy keeps the template's MIT license (ADR-0011 overrides its product
checklist's GPL conversion). No dependency or system configuration changes.

The proven Utility monorepo seams are carried forward: isolated ExternalProject,
offline vendored dependency paths, LinkMonorepo, core-params/ParamStore/
ClapParams/FlatSemantic, canonical JSON state, gesture backpressure handling,
semantic conformance and concurrent patch/process tests. The template DSP and
CLI are removed; `engine/` is the single SDK-free float/double DSP implementation.
Renderer, widgets, window, wrapper patches, fonts and host-contract probes stay
as shipped apart from naming and vendor namespace.

Product-owned changes: Editor/Theme, parameter adapter, CLAP product sections,
semantic_rt/editor_input tests and the host_smoke restart section, GUI preview, CMake integration. The 760 × 450
editor shows a transfer curve with a telemetry peak dot, a deterministic static
harmonic spectrum of a −18 dBFS RMS test sine (not programme FFT telemetry), and
all eight writable parameters. Values sit below dial pointers; branding and
product name occupy separate lines. Mouse, keyboard, wheel, reset and focus-loss
edits are balanced gestures. Preview scenarios are `default` and `hot` (fold,
24 dB, bias 30%). Preview meters run through the real engine.

Oversampling is a stepped, non-automatable parameter. `x1`/`x2`/`x4`/`x8` are
patch keys (core-params requires alphabetic choice keys); labels are 1x/2x/4x/8x.
An active change updates the saved/requested setting, coalesces request_restart,
and keeps the prepared factor and latency until reactivation. The engine
preallocates fixed-capacity histories; prepare computes the reference Gaussian
RMS compensation tables. No DSP topology is rebuilt on the audio thread.
Latency is 1/73/81/84 base samples, including ADAA and its integer padding;
dry mix is delayed identically. Input/output readonly peak parameters publish
at 50 ms intervals. No template binary state existed for this new identity;
only canonical framed JSON is accepted transactionally.

Shared addition: core-dsp's linear-phase half-band Oversampler with tests for
passband, images/aliases, integer delay, reset and partition independence.
Orrery's own minimum-phase decimator is untouched. Curve equations, exact tone
interpretation, compensation model and measured limits are in stock-effects §5
and the skill guide. Factory recipes live in the guide and are patch-tested.

Verification is Linux CLAP and X11/OpenGL. VST3/AUv2 and REAPER acceptance remain
with the orchestrator; the template wrapper code is retained but those platforms
are not claimed tested here. STATUS.md is orchestrator-owned.

The product-owned host_smoke restart regression waits for reactivation before
expecting a new latency, and checks pending latency stability and coalescing
for every factor. The template's old section expected an active delay change;
its shared host_contract.h remains unchanged.

Linux hosts must implement clap.timer-support, as in the unchanged template
Window.cpp. The optional --no-host-timer probe cannot map/repaint its child
without that host pump; this unsupported path is reported rather than altered.

Review refinement: 145/33/25-tap stages protect the audible band without
requiring rejection between 20 kHz and Nyquist in the Kiln alias test.
The stage pass/stop bands and integer delays are in oversampler.h.
CI tests retain property coverage with aggregated sample checks, shorter
observations and representative grids. Dense measurements are opt-in:
`cmake --build --preset dev -j6 --target drive-engine.measure drive-bench`,
then run the binaries in `build/dev/plugins/drive/engine/`. The measurement
runner is not registered with CTest; the separate CPU runner uses -O2 for
processor.cpp and oversampler.cpp. No test timeout override remains.
