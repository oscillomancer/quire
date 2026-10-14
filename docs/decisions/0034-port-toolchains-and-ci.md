# ADR-0034: Port toolchains and on-demand CI

**Status:** Accepted, 2026-10-14. Amends ADR-0003.

## Context

The macOS and Windows port needs branch CI before changes can reach the
default branch, and incremental gates while Linux-only services are ported.
The repository is private, so hosted runner cost makes targeted runs useful.

## Decision

- Preserve existing Linux presets and their target/test coverage. Hosted
  Linux uses Clang 22 on Ubuntu 24.04 and the full dev preset.
- macOS is Apple Silicon, minimum 14.4, AppleClang first. Use Homebrew LLVM
  only if a demonstrated libc++ gap requires it.
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
