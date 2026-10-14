# ADR-0036: Native in-process plugin hosting and supervised scanning

**Status:** Accepted, 2026-10-14. Amends ADR-0013, ADR-0017 and the temporary
hosting gates in ADR-0034/0035. The audio bridge protocol is unchanged.

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
independently of the engine and audio bridge. macOS can also build the sandbox
and offline renderer. The engine remains gated pending separation of Linux
JACK scheduling/affinity from device-independent services. Windows sandbox
and engine retain the audio-file gate. Linux-only `/proc`, signals, subreaper,
Unix-script and bridge protocol probes remain on Linux, with native fixture
coverage of loading, state, audio, supervised crash/hang/cache and child cleanup.
Native CI results and any template boundary failures are recorded in STATUS.

## Alternatives considered

Porting the audio bridge as part of discovery would couple separate stages.
Running scanner probes in the host would break background-scan thread and
crash-isolation contracts. Fake remote processing would hide unsupported modes.
