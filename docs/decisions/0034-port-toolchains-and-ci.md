# ADR-0034: Port toolchains and on-demand CI

**Status:** Accepted, 2026-10-14. Amends ADR-0003.

**Gate amendment, 2026-10-14:** [ADR-0035](0035-headless-platform-layer.md)
removes the native core-base and control/MCP gates. Remaining hosting-dependent
services use `SUPPLE_HOST_SERVICES`; the Windows audio-file gate is unchanged.

**Hosting amendment, 2026-10-14:** [ADR-0036](0036-native-plugin-hosting.md)
adds native in-process loaders and a standalone supervised scanner, while
retaining the Linux audio bridge and the remaining engine/device/GUI gates.

**Audio-file amendment, 2026-10-14:** native Windows recording/recovery and
RF64 publication lift the POSIX audio-file gate; see engine.md's Native audio
files contract. Device audio, bridge and GUI remain separate gates.

**Packaging amendment, 2026-10-24:** [ADR-0048](0048-packaging-and-distribution.md) adds installed layouts and on-demand package CI.

## Context

The macOS and Windows port needs branch CI before changes can reach the
default branch, and incremental gates while Linux-only services are ported.
The repository is private, so hosted runner cost makes targeted runs useful.

## Decision

- Preserve existing Linux presets and their target/test coverage. Hosted
  Linux uses Clang 22 on Ubuntu 24.04 and the full dev preset.
- macOS is Apple Silicon, minimum 14.4, AppleClang and the system libc++.
  The review on 2026-10-14 replaces the initial Homebrew LLVM fallback:
  shipping a second libc++/libunwind beside system-runtime plugins creates
  packaging and hosting liabilities. Vendored fast_float (MIT option) supplies
  float/double parsing through one core-base helper on every platform;
  integer parsing remains in the standard library. Fix further compiler gaps
  portably or report the tradeoff instead of substituting a bundled runtime.
- Windows is x64, minimum Windows 10 1803, clang-cl with the Windows SDK
  and Ninja. Forward GNU-style determinism and warning flags to Clang.
- Determinism is required within each platform, including RT/offline
  equivalence when the engine is enabled. Floating-point results need not
  be bit-identical across platforms.
- CI runs only on workflow_dispatch or deliberately pushed ci/** tags.
  Annotated tags select platforms; builds cancel superseded work per branch
  and platform. Cache dependencies, cap parallelism at six, and delete tags
  after runs. No packages or installers are produced.
- Define temporary Linux service and POSIX audio-file gates centrally in
  cmake/Platforms.cmake, with their complete list in CONTRIBUTING.md.
  Keep portable targets and tests enabled. Do not replace RT services with
  implementations that violate their contracts.

## Consequences

Branch builds can run before the workflow reaches main. Port steps remove
named gates as real platform implementations land. Passing portable tests
is not evidence that the headless DAW or device audio works on that platform.

**2026-10-16 note:** The Windows minimum is now Windows 10 1903 (ADR-0044).
