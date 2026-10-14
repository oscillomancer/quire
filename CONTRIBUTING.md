# Contributing

Thanks for looking at Quire. The engineering rules in `AGENTS.md` apply to
every contribution, whether you write it yourself or with a coding agent.
This page covers the process around them.

## Reporting a problem

Open an issue with:

- what you did, what you expected and what happened;
- the Quire commit you built, your distribution, and your audio setup
  (PipeWire or JACK, buffer size, sample rate);
- the smallest project file that shows it, when one does (Quire projects
  are JSON, with their media in the `media/` folder beside them);
- for crashes, the terminal output and a backtrace if you can get one.

Plugin problems are easier to pin down with the plugin's name, format
(CLAP or VST3), version and isolation mode.

## Proposing a change

For anything larger than a fix, open an issue first and describe the
change. Design changes that touch a file format, a command's schema, the
control protocol or a library's public interface need a spec update, and a
change of direction needs an ADR (`docs/decisions/README.md` explains the
format).

A pull request should:

- do one thing, with commits staged from explicit paths and subjects in the
  form `Area: what changed`;
- include tests that fail without the change (fixes) or cover it
  (features);
- update the spec, the generated files (`tools/gen/update-generated.sh`)
  and `STATUS.md` where they are affected;
- pass `tools/ci/run-all.sh` locally; say in the description which presets
  you ran if you could not run them all;
- state what you verified and how, including anything checked by hand in
  the GUI.

Code generated with an agent is welcome on the same terms: you are
responsible for every line, and you must have run what you claim works.

## Licensing of contributions

Contributions use the license of the component they change: GPL-3.0-or-later
for the DAW, MIT for plugins and their shared libraries (ADR-0033).
Contributions must be original and must never include code copied from GPL
or AGPL projects, or from REAPER (see `AGENTS.md`). The vendored ASIO SDK
is the sanctioned exception for Windows DAW integration.

## Continuous integration

`.github/workflows/build.yml` runs only on demand. Once the workflow is on
`main`, use Actions → Build and test → Run workflow, or
`gh workflow run build.yml --ref <branch> -f platforms=macos`.
Choices are `linux`, `macos`, `windows`, and `all` (the default).
Ordinary branch pushes never start a build.

Before the port reaches the default branch, push a deliberate CI tag:

```sh
git push origin port-ci
git tag -a ci/port-ci-1 -m 'platforms=all'
git push origin refs/tags/ci/port-ci-1
gh run list --workflow build.yml
gh run watch <run-id> --exit-status
gh run view <run-id> --log-failed
git push origin :refs/tags/ci/port-ci-1
git tag -d ci/port-ci-1
```

Use `ci/<branch>-<n>` with a new number each time. Annotate with
`platforms=linux`, `platforms=macos` or `platforms=windows` to target only
one platform; unannotated tags run all. Superseded builds cancel per branch
and platform, including across different tag numbers. Delete remote CI tags
once their runs finish. Dependencies and Linux compiler output are cached;
run only the platforms needed for the change.

GitHub isolates caches by ref: a fresh CI tag cannot restore a cache saved
under another tag. Caches help same-ref reruns and, after integration,
manual runs on the same branch. Linux ccache also reuses shared compilation
within each run's isolated plugin builds. See GitHub's
[cache access restrictions](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache).

Linux (`ubuntu-24.04`, Clang 22) builds the existing `dev` preset with all
its tests, under Xvfb and software OpenGL. SDL3 and the pinned CLAP validator
are built and cached in the runner's workspace; Ubuntu 24.04 predates SDL3.
macOS (`macos-15`, arm64) uses
AppleClang and the system libc++, with deployment target 14.4. Its build preset
uses two jobs, also enforced explicitly by the workflow, to keep the engine
command translation units within the hosted runner memory budget. The root
project uses `-gline-tables-only` in this preset: assertions and unoptimized code
remain enabled, with file/line stack traces. For full variable/type debugging
on a larger machine, configure with `-DCMAKE_CXX_FLAGS_DEBUG=-g`. Isolated stock
projects retain their own Debug flags. Homebrew
supplies Ninja/SDL3, while curl and zlib come from the SDK. Float/double
parsing uses the vendored MIT fast_float library through core-base on every
platform; integer parsing remains `std::from_chars`. This avoids the Apple
libc++ floating-point parsing gap without shipping a second C++ runtime.
AppleClang's advisory `-Wperf-constraint-implies-noexcept` is suppressed
when supported for the existing RT signatures; `nonblocking` annotations
and the other warnings remain enabled.
Windows (`windows-2022`, x64) uses clang-cl, Ninja
and the Windows SDK, with vcpkg SDL3/zlib/curl and a binary cache. Windows
10 1803 is the minimum. No packaging or installers run here. Local platform
commands are `cmake --workflow --preset macos` or `windows`; the latter
requires `VCPKG_ROOT` and an initialized Windows SDK environment.

If `build/macos` was configured with the earlier Homebrew LLVM toolchain,
start with `cmake --fresh --preset macos` to discard its compiler/runtime
cache, then build and test with the `macos` presets.

### Temporary port gates

`cmake/Platforms.cmake` defines the boundaries. Later port steps remove
them as implementations and tests become available. Linux coverage is
unchanged. macOS and Windows currently build/test JSON/schema/params/patch,
core-base native waits/wakes/thread setup, command bus, control-protocol/MCP,
Lua, DSP/Console 9/analysis, MIDI/notation/timeline/optimizer,
session model, plugin support, all stock SDK-free DSP engines, and the
script compiler/runtime and suite semantic layer. Audio-file tests run on all three platforms. Native core-base sources/tests, JSON-RPC control transport/model
loop and standalone MCP/proxy no longer share the hosting gate (ADR-0035).
Linux-specific descriptor/permission/signal probes and memory-residency/rlimit
tests remain guarded with portable boundary coverage alongside them.
These gates are explicit, with no no-op RT implementations:

| Gate | Off on | Reason / follow-up |
|---|---|---|
| `SUPPLE_HOST_SERVICES`: audio bridge | macOS, Windows | Shared memory, cross-process wake and native IPC; bridge port step |
| JACK/fake JACK and hardware capacity benchmark | macOS, Windows | Native device backend is a later port; requests return backend.unsupported |
| Same gate: DAW GUI/agent and source-export integration tests | macOS, Windows | GUI depends on engine/X11; stock wrappers and conformance/reset tests now enabled separately |
| POSIX evaluation harness tests | Windows | Unix executable fixtures, symlinks and process groups |

Research GUI spikes and host fuzzers reject configuration on non-Linux
platforms rather than silently building incomplete targets. Renders must
be deterministic on each platform; cross-platform bit identity is not
required (ADR-0034).

ADR-0036 enables native CLAP/VST3 hosting, the standalone supervised scanner,
stock plugin builds and their conformance/reset checks on macOS/Windows.
The existing Linux bridge/protocol, `/proc`, subreaper and shell-script probes
remain Linux-only. Native tests cover fixture loading, state/audio, crash/hang
isolation, cache and process capture/deadlines. Windows external validator
shell tests are omitted; native product host-smoke tests still run. The macOS
external validator entries skip when the optional validator is absent; the
product, semantic and host checks still execute.

Windows stock builds enable the CRT math constants with _USE_MATH_DEFINES in
platform build glue. Product-owned semantic stress tests use a native loader;
template host/probe/shared-stack sources are unchanged.

Apple libc++'s missing jthread/stop-token facilities are supplied through the
core-base non-RT job layer, retaining standard types elsewhere. The VST3 loader
uses Objective-C++20 with ARC. The stock projects' existing AUv2 builds use the
vendored Apache-2.0 AudioUnitSDK at clap-wrapper's pin; no downloads or template
source changes are required.
