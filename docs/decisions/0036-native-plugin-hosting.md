# ADR-0036: Native in-process plugin hosting and supervised scanning

**Status:** Accepted, 2026-10-14. Amends ADR-0013, ADR-0017 and the temporary
hosting gates in ADR-0034/0035. The audio bridge protocol is unchanged.

**Audio-file amendment, 2026-10-14:** native Windows recording/recovery and
RF64 publication lift the POSIX audio-file gate; see engine.md's Native audio
files contract. Device audio, bridge and GUI remain separate gates.

**DSP link correction, 2026-10-14:** Windows engine linking exposed a duplicate
Oversampler::reset symbol between core-dsp and the script runtime's template DSP.
The runtime imports template DSP under a private namespace (without editing the
template), preserving both layouts and avoiding ELF symbol interposition too.
A combined-library regression exercises both reset paths; script APIs and numeric
behavior are unchanged.

## Context

In-process hosting and discovery must work before the Linux audio bridge,
device backends and editor embedding are ported. Scanning needs crash and
hang isolation even when the audio bridge is unavailable.

## Decision

- CLAP uses dlopen on Linux, a bundle's CFBundle executable on macOS, and
  LoadLibraryW on Windows. Entry init receives the original canonical module
  path in UTF-8, including the bundle directory on macOS. CLAP unload and
  shared-instance ownership are unchanged. VST3 uses the existing pinned MIT
  SDK's native module loaders; the deliberate lifetime-long cache is retained.
- Standard user/system CLAP/VST3 directories follow the platform. Environment
  search paths use semicolons on Windows and colons on POSIX. Core-base paths
  supplies home/cache roots. Discovery treats CLAP bundle directories as single
  modules, and fingerprints all regular files within bundles.
- A standalone `<slug>-scanner --scan <module>` implements the existing scan
  reply schema. On Linux the existing bridge scan mode remains the default.
  Other platforms locate the scanner next to the application or in the build
  tree. Both redirect plugin stdout away from the private protocol stream.
- `ChildProcess` in clap-host owns a child and its process tree. `spawn(path,
  arguments)` supplies null stdin/stderr and captured stdout; `collect(deadline,
  maxBytes)` returns output, exit code and abnormal-exit classification. It
  reports structured spawn/I/O/wait/timeout/output-limit errors. Destruction
  kills the tree and reaps the leader, including after failures. `killTree()`
  permits explicit cancellation. No shell is invoked. The helper is available
  for the later bridge port; custom IPC handle passing is a future extension.
- POSIX retains a separate process group, nonblocking stdout and an unreaped
  leader (WNOWAIT) until cleanup. Linux closes all descriptors above stdio;
  macOS uses POSIX_SPAWN_CLOEXEC_DEFAULT and explicit stdio file actions.
  Windows restricts handle inheritance with STARTUPINFOEX, creates the child
  suspended, assigns a Job object before resuming, and terminates the Job on
  cleanup. The job also uses KILL_ON_JOB_CLOSE. Capture uses native reads and
  core-base Waitable waits, capped at 10 ms while observing stdout/exit; a
  descendant retaining stdout cannot extend the absolute deadline.
- On non-Linux systems isolation requests fail with `host.bridge.unsupported`,
  path `/isolation`, and a hint to choose `in_process`. The remote backing is
  abstract and cannot be instantiated; there is no substitute RT processing.
  Editor opening similarly returns `host.editor.unsupported` until the GUI port.
- Stock product orchestration builds the existing template copies unchanged.
  Orrery uses core-dsp's DenormalGuard, including arm64 FPCR FZ/restoration.
  Linux-only loader/process/descriptor tests retain their existing coverage.

## Consequences

Hosting, scanner, stock wrappers and host conformance/reset checks can build
independently of the engine and audio bridge. macOS also builds the engine library/CLI, dummy backend, sandbox and offline
renderer. JACK/fake JACK and the hardware benchmark remain Linux-only; an
explicit JACK request elsewhere returns backend.unsupported with a
/backend path and dummy hint. Windows sandbox and engine retain the audio-file
gate. Native engine tests exercise the existing RT/offline equivalence contract
in the callback and anticipative executors. Linux-only `/proc`, signals, subreaper,
Unix-script and bridge protocol probes remain on Linux, with native fixture
coverage of loading, state, audio, supervised crash/hang/cache and child cleanup.
Native CI results and any template boundary failures are recorded in STATUS.

## Alternatives considered

Porting the audio bridge as part of discovery would couple separate stages.
Running scanner probes in the host would break background-scan thread and
crash-isolation contracts. Fake remote processing would hide unsupported modes.

## Native build integration (2026-10-14)

Native CI exposed two Apple libc++ gaps: filesystem clock ticks use 128 bits,
and jthread/stop-token facilities are absent. Scanner fingerprints preserve all
decimal timestamp digits without narrowing. Core-base StopToken/JoiningThread
alias the standard types where supported and provide an original non-RT fallback
for Apple, with synchronous callback lifetime and join behavior documented in
core-base.md. Standard and forced-fallback tests run independently on Linux too.
The product-owned Analyzer analysis worker uses the same helper. The GUI still
needs its own conversion when its gate is lifted. The macOS build preset limits
compilation to two jobs: six concurrent engine command translation units
stopped making progress on the hosted runner after the engine gate was lifted.

The VST3 macOS loader compiles with ARC and C++20. Existing stock projects
also request AUv2 on macOS; clap-wrapper's pinned AudioUnitSDK-1.1.0 (Apache-2.0,
commit 53a9a2008aae7fb1b0a9f093dd523b9b12f6c0d9) is vendored and passed through
AUDIOUNIT_SDK_ROOT. Template/shared-stack sources remain untouched. Windows test
library paths use semicolons; product semantic tests use native loaders.
Windows discovery includes legacy single-file VST3 modules. CLAP directory
bundles are recognized only on macOS, preserving Linux recursive search behavior.

Windows scanner protocol handles are explicitly non-inheritable; scan children
suppress fault/critical-error dialogs, and real-exception fixtures exercise crash
classification. Host scan diagnostics retain their Linux messages and hints.
Non-Linux engine.start defaults to dummy; explicit JACK requests return
backend.unsupported. Plugin-file DSP exports remain Linux-only and return
an actionable dsp.platform error elsewhere; source exports remain available.

Windows isolated plugin builds use a configuration-specific library output
path matching their declared asset folder, then stage only the named product
and notices into the shared stock directory. This avoids clap-wrapper's extra
CLAP subfolder without changing it or the template, and keeps internal test
modules outside the stock catalogue. Player staging uses the same flat output
contract; macOS continues to copy its complete bundle.

Product semantic concurrency tests use native module loaders and run on every
platform. Tail/unload probes that use dlopen or /proc remain Linux-only.

The macOS workflow also supplies an explicit two-job CLI limit and matching
CMAKE_BUILD_PARALLEL_LEVEL. A cancelled native run listed twelve clang processes
with only preset jobs=2 while the workflow environment specified six. The
explicit CLI bound avoids relying on preset/environment precedence.

A completed two-job native build took 59 minutes, leaving no margin for tests;
the large engine command translation units dominated that build. The macOS
preset uses -gline-tables-only to reduce debug emission cost and compiler memory
for the root project while
retaining Debug assertions and unoptimized code. File/line stack traces remain
available; full type/variable debug info can be restored with
-DCMAKE_CXX_FLAGS_DEBUG=-g on a larger machine. Stock isolated builds keep their
existing flags. The fallback implementation's private namespace is stop_detail
to avoid collisions with component detail namespaces when both are imported.

Native conformance found that a directory can look like an empty successful
ifstream read on macOS. The product-owned script catalog requires a regular
file before its bounded read; the shared template is unchanged. The MCP
real-synth session keeps all raw protocol/render/optimization assertions on
native systems, while its exact Linux numeric transcript remains a Linux drift
check. Native logs retain the reference diff for review (per-platform render
determinism, ADR-0034).
