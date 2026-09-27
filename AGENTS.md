# Instructions for agents working in this repository

Quire is an LLM-native DAW plus a suite of LLM-programmable stock plugins,
built by Supple Audio. Read `ARCHITECTURE.md` and the ADRs in
`docs/decisions/` before designing anything; read the spec in `docs/specs/`
for any component you implement. `STATUS.md` says what is in flight.

The orchestrator (lead engineer) owns architecture, interfaces and integration.
If a task brief conflicts with an ADR or spec, stop and report the conflict
rather than picking one silently.

## Hard rules

1. **Licensing.** All code is original. Never copy code from Ardour or any GPL
   /AGPL project, and do not read GPL source while implementing. Research of
   GPL source produces prose notes in `docs/research/` only, and a *different*
   agent implements from those notes. Dependencies must be MIT, BSD-2/3, ISC,
   Zlib, Apache-2.0, BSL-1.0, CC0/public domain or Unlicense (fonts: OFL-1.1), must be listed
   in `THIRD_PARTY.md`, and live under `third_party/` with their license file.
   `tools/license-check` fails the build otherwise. LGPL/MPL need an approved
   entry in `HUMAN_REVIEW.md` first.
2. **REAPER is proprietary.** Study it only by using it, reading its project/
   config files and its public scripting docs. Never decompile it, and never
   copy its themes, icons or assets.
3. **Never modify the plugin template** at `~/Projects/audio-plugins/template`
   or Nate's product repos next to it. Copy/instantiate the template into
   `plugins/` the way its `AGENTS.md` describes.
4. **No system configuration changes.** No sudo, no package installs outside
   user space, no PipeWire/WirePlumber/kernel/udev/rtirq edits. Report
   environment problems instead.
5. **Real-time safety is absolute.** Code on the audio path never allocates,
   frees, locks, blocks, logs synchronously, throws, or touches files or the
   network. State reaches it through lock-free queues or atomic pointer swaps.
   Audio-path code is tested under RealtimeSanitizer (`rtsan` preset).
6. **The product name lives in one place.** `cmake/Product.cmake` defines it;
   C++ gets it from the generated `supple/product.h` (`supple::product::kName`
   etc.). Never hard-code "Quire"/"quire" in code, binary names, paths, socket
   names or UI strings. C++ code lives in namespace `supple::` (the vendor),
   not a product-named namespace. Docs may say "Quire".
7. **This machine is Nate's workstation.** Build with at most `-j6`. Do not
   leave processes running. Do not open the audio device except in tests that
   need it, and release it afterwards.

## Code conventions

- C++20, CMake + Ninja, builds with both Clang and GCC, warnings clean with
  `-Wall -Wextra -Wpedantic` on our targets (off for `third_party`).
- Formatting: `.clang-format` at the repo root. Run `tools/ci/format.sh`.
- Tests: doctest, one test executable per library at least, registered with
  CTest. Name tests after the library (`core-json.tests`).
- Errors that cross the command bus or a file-format boundary are structured
  (`code`, `message`, `path`, `hint`) and tell the caller how to fix the call.
- Text formats are deterministic: canonical ordering, a `schema`/`version`
  field, byte-identical output for identical input.
- Library layout: `libs/<name>/include/supple/<name-without-core->/...h`,
  `libs/<name>/src/`, `libs/<name>/tests/`. Public headers are the interface;
  keep them small and documented.
- No exceptions across library APIs; use `supple::Result<T>` from core-base.
  Exceptions may be used internally in non-RT code if they never escape.

## Build and test

```sh
tools/ci/build.sh            # configure + build + test, default dev preset
cmake --workflow --preset dev
tools/ci/run-all.sh          # every preset: dev, gcc, asan, ubsan, tsan, rtsan + license + format
```

Phase 3 acceptance in REAPER (only when the plugin is installed in a CLAP path
REAPER scans, e.g. ~/.clap; opens the audio device):
`tools/reaper/load-test.sh <plugin display name>`.

CTest labels: `rt` (audio-path tests, run everywhere), `tsan-skip` (excluded
from the tsan preset: timing-heavy tests TSan makes impractically slow), `slow`
(minutes-long end-to-end tests: run in dev, gcc and asan only). A test that
depends on wall-clock time must not decide pass/fail by it.

## Definition of done

Code, tests, docs updated, `STATUS.md` updated (by the orchestrator unless the
brief says otherwise), no new license issues, and reviewed by an agent other
than the author. Never report something as working that you did not run.
Report: files changed, exact verification commands and their results, open
questions.
