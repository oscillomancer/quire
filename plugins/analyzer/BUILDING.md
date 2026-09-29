# Building and validation

Dependencies are fetched at configure time and pinned in CMake. No git
submodules are required.

## macOS

Requirements: Xcode command-line tools, CMake 3.21 or newer, Git, and a current
macOS SDK.

Release artifacts target macOS 11.0 by default, the first release that can run
their arm64 slice. Override `CMAKE_OSX_DEPLOYMENT_TARGET` explicitly when
building a separate Intel-only compatibility artifact.

```sh
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

Outputs are under `build-cmake/analyzer_assets/`. macOS builds `Analyzer.clap`,
`Analyzer.vst3` and `Analyzer.component`. Metal is the default. To exercise the
OpenGL plugin path explicitly:

```sh
cmake -S . -B build-gl -DCMAKE_BUILD_TYPE=Release -DANALYZER_METAL=OFF
cmake --build build-gl -j
```

Install all formats together only when wanted:

```sh
cmake --build build-cmake --target analyzer-install
```

`analyzer-install` prepares and ad-hoc signs the AUv2 bundle before copying it,
so the installed component carries clap-wrapper's AudioComponents plist and a
valid signature rather than CMake's generic plist.

## Linux

Typical Debian/Ubuntu build dependencies:

```sh
sudo apt install build-essential cmake git libx11-dev libxcursor-dev \
  libxrandr-dev libxext-dev libgl1-mesa-dev
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

Linux builds CLAP and VST3 using X11/OpenGL. This was first actually compiled
on 2026-08-25 (Arch, GCC 16, CMake 4.4, Mesa): both formats build and install,
ctest passes, and the offline GUI preview renders correctly. Note that a CLAP
is a bare `.so` here rather than a bundle directory as on macOS; only the VST3
is a folder. Still test the result in real Linux hosts before claiming support:
a successful build does not validate X11 embedding or host timer behaviour.

## Windows

Use a current Visual Studio developer prompt with CMake and Git available:

```powershell
cmake --preset release
cmake --build --preset release --config Release
ctest --preset release -C Release
```

The custom NanoVG backend dynamically loads post-OpenGL-1.1 entry points on
Windows. Validate the CLAP and VST3 editors in real hosts on an actual Windows
machine.

## Host-level CLAP probe

On macOS, load the binary inside the built bundle:

```sh
./build-cmake/analyzer-clap-probe \
  ./build-cmake/analyzer_assets/Analyzer.clap/Contents/MacOS/Analyzer \
  200 --mono --resize --reopen
```

Useful stress switches are `--no-host-timer`, `--destroy-hot`, `--drop-parent`,
`--mono`, `--resize`, and `--reopen`. `--mono` selects the declared mono port
configuration before activation. The exact shared-library path inside CLAP
bundles differs on Linux and Windows.

## Tests

`ctest` runs the following checks, all expected to pass before a release:

- `core` -- the DSP, offline and SDK-free.
- `editor-input` -- gesture and keyboard handling, without a renderer.
- `wrapper-automation` -- that the clap-wrapper VST3 automation patch is still
  applied to the pinned checkout.
- `wrapper-timer` -- that the VST3 timer-floor patch is still applied.
- `auv2-bundle` -- that the built AU carries clap-wrapper's AudioComponents
  plist and a valid signature, rather than CMake's generic plist that no host
  reads (macOS only).
- `host-smoke` -- a real host that loads the built `.clap` and drives it the
  way a DAW does: parameters, processing, latency and restart behaviour, state
  round trip, teardown. Port/text/render checks are shared in `host_contract.h`;
  lifecycle tests can be product-specific. A new product must set
  `HOST_SMOKE_PLUGIN_ID` and `HOST_SMOKE_PLAIN_PARAM` (the display name of a
  parameter that must never change latency), plus `HOST_SMOKE_LATENCY_PARAM`
  if it has a parameter that does.
- `host-smoke-mono` -- the same lifecycle and contract checks on the negotiated
  mono configuration.

## Sanitizers and warnings

Analyzer-owned core/plugin targets build with `-Wall -Wextra -Wpedantic` (or
`/W4`) by default. The tools-only sanitizer preset keeps SDK and renderer code
out of the run and exercises the allocation-free DSP core under ASan and UBSan:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers -j
ctest --preset sanitizers
```

## Offline audio rendering

```sh
./build-cmake/analyzer-cli input.wav output.wav \
  --input-gain 3 --output-gain -1 --block 257
```

The offline renderer calls the same SDK-free processor as every plugin format.
Vary `--block` when investigating history or boundary bugs. Extend this tool
with product-specific traces and independent measurements rather than making a
DAW meter the only source of truth.

## Visual preview

```sh
./build-cmake/analyzer-gui-preview 180 analyzer-gui.ppm
```

This renders the real editor and is useful for layout review. It is not a host
integration test and cannot validate wrapper timers, parent views or automation.

## CLAP host-contract regression checks

`ctest --test-dir build-cmake -R host-smoke --output-on-failure` runs the
built CLAP through `test/host_smoke.cpp`. Its shared `test/host_contract.h`
checks port precision declarations, effect in-place pairing (or no pairing for
an instrument), and parameter text at defaults, bounds and interior values.
Small stepped ranges are enumerated; non-finite/overflowing text must be
**rejected**, not merely clamped to a finite endpoint.

Audio comparisons render identical inputs and events on fresh, identically
configured instances. Float/double renders compare every sample; effects also
compare separate buffers against per-channel in-place buffers, with distinct
left/right signals. Instruments receive note events and fixed seeds rather than
passing this check with silence. These are host-contract regressions, not a
replacement for format validators or real DAW/editor testing.

Mono and stereo smoke runs are registered with CTest. Configuration IDs
are discovered from the advertised layouts rather than assumed.

## Pre-release matrix

- clean Release and Debug configure/build;
- all registered ctest tests, plus the sanitizer preset;
- CLAP probe with host timer and fallback timer;
- mono/stereo, float/double, block sizes 1/64/512/4096;
- 44.1/48/88.2/96/176.4/192 kHz where supported;
- editor resize, close/reopen, hot teardown and state recall;
- CLAP/VST3 scanners on Linux, macOS and Windows; `auval` on macOS;
- automation while playing and stopped;
- plugin IDs, vendor, version, description, AU codes and license notices.

## Native Linux embedding probe

With an X11/XWayland display available, run:

```sh
./build-cmake/analyzer-linux-host-probe \
  ./build-cmake/analyzer_assets/Analyzer.clap /tmp/analyzer-host.ppm
```

This product-independent probe creates a real X11 parent, verifies the mapped
child, captures the embedded pixels, and exercises resize, hide/show, three
create/destroy cycles and timer cleanup while audio runs on a separate thread.
It exits nonzero on failed assertions. The existing `--resize`/`--reopen` flags
on `clap-probe` exercise macOS only; use this probe for Linux.
`--no-host-timer` is diagnostic: it does not prove fallback repaint liveness.
