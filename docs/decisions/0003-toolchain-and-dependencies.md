# ADR-0003: Toolchain, dependency vendoring and license gate

**Status:** Accepted, 2026-09-26.

2026-10-14: [ADR-0033](0033-port-toolchains-and-ci.md) amends the platform
toolchains, CI triggers and cross-platform determinism requirement.

## Context

C++20, CMake + Ninja, Clang and GCC, sanitizers including RealtimeSanitizer
are required (build prompt 2.4, 6). A clean clone must build with one command
(Phase 0 acceptance). Every dependency must be permissively licensed and
declared, and a script must fail the build otherwise (2.2). The machine has
Clang 22.1.8 (with `libclang_rt.rtsan`), GCC 16.2.1, CMake 4.4.3, Ninja 1.13.2.

## Decision

- **Language:** C++20, extensions off. No exceptions across library APIs:
  `supple::Result<T>` / `supple::Error` (core-base) everywhere, so errors are
  structured end to end (code, message, JSON-pointer path, hint, source
  location, details). C++23 `std::expected` is not used to stay on C++20.
- **Build:** CMake ≥ 3.25 with `CMakePresets.json`: `dev` (Clang Debug,
  `-Werror`), `release`, `gcc`, `asan`, `ubsan`, `tsan`, `rtsan`
  (RelWithDebInfo, `-fsanitize=realtime`). Workflow presets make one command
  configure+build+test: `cmake --workflow --preset dev` (wrapped by
  `tools/ci/build.sh`). `tools/ci/run-all.sh` is local CI: every preset plus
  license and format checks. Build and test jobs are capped at 6 in presets (development happens on a
  working audio workstation).
- **RT enforcement:** audio-path entry points are annotated `SUPPLE_RT`
  (`[[clang::nonblocking]]`). Under the `rtsan` preset, any allocation, lock
  or syscall reached from them aborts the test. `tests/rt-safety/
  rtsan_selfcheck` proves the gate is live (it must be killed).
- **Dependencies are vendored** under `third_party/<dir>/` with their
  license file, pinned by commit in `third_party/manifest.json`. No network at
  configure time for the core build. (Template-derived plugins keep the
  template's pinned FetchContent stack; those names must appear in the
  manifest too — the gate scans `FetchContent_Declare`.)
- **License gate:** `tools/license-check/check.py` runs as an `ALL` build
  target and a CTest test. It fails on undeclared `third_party/` dirs,
  disallowed licenses, missing license files, dependencies absent from
  `THIRD_PARTY.md`, undeclared FetchContent/ExternalProject names, and
  (L/A)GPL SPDX identifiers in first-party code.
- **Initial dependency set** (verified licenses, see `THIRD_PARTY.md`): CLAP
  1.2.10 (MIT), doctest 2.5.3 (MIT, tests only), dr_libs (Unlicense/MIT-0),
  pocketfft C++ (BSD-3). Planned: Lua 5.4 (MIT), NanoVG (Zlib), Pugl (ISC)
  and/or SDL3 (Zlib), clap-wrapper (MIT), VST3 SDK (MIT as of 3.8.0 —
  re-verify at use), libFLAC encoder (BSD-3) for FLAC render.
- **Fonts** are assets, not code. SIL OFL-1.1 fonts may be bundled (OFL
  permits bundling with software under any license; the fonts just cannot be
  sold on their own). This covers Inter and IBM Plex, which the plugin template already ships. Every font keeps its license file next to it.
- **System libraries** are dynamically linked and listed under
  `system_libraries` in the manifest: SDL3 (Zlib), libjack from pipewire-jack
  (MIT; dlopen'ed), libpipewire (MIT), alsa-lib (LGPL, but an OS library,
  which the policy explicitly allows when dynamically linked), libX11/libGL.
- **Own implementations** where the dependency is small or its behaviour must
  be exactly ours: JSON (error locations, ordered keys, canonical output),
  EBU R128 loudness (validated against Tech 3341/3342 signals generated in
  tests), SMF I/O, resampling (polyphase windowed-sinc) and the optimizer.
- **Tests:** doctest; one executable per library; CTest labels per library
  plus `rt` for audio-path suites (run under rtsan in CI).
- **Formatting:** `.clang-format` (Google base, 4-space indent, 100 cols,
  matching the template's style); `tools/ci/format.sh [--check]`.
- **Python** only for tooling (license gate, eval harness, doc generation),
  never in shipped runtime paths.

## Consequences

Clean clones build offline in seconds. Upgrading a dependency is an explicit
commit that touches `manifest.json` and `THIRD_PARTY.md`. Owning JSON costs a
few hundred lines but gives LLM-quality error messages
(`line 3, col 17: expected ',' or '}' after member "cutoff"`), which matter for
patch and notation authoring.

## Alternatives considered

- FetchContent for everything (as the template does): needs network, slower
  clean configures; kept only for the template stack inside plugins.
- nlohmann/json (MIT): ubiquitous, but heavy compile times and byte-offset
  errors; RapidJSON: its repository includes the non-free "JSON License"
  file; simdjson: parse-only.
- Catch2 (BSL-1.0): heavier than doctest for no gain here.
- libebur128 (MIT): viable, but we must validate against EBU signals anyway,
  and owning it lets the analysis layer share K-weighting/oversampling code.

## Amendment, 2026-10-14

[ADR-0033](0033-daw-and-plugin-licensing.md) amends the license gate and
dependency policy: GPL DAW code, MIT plugins and shared libraries, and
a sanctioned Windows DAW-only ASIO SDK dependency.
