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
once their runs finish. Dependencies are cached; avoid repeated full runs
on this private repository (macOS and Windows consume more Actions minutes).

Linux (`ubuntu-24.04`, Clang 22) builds the existing `dev` preset with all
its tests, under Xvfb and software OpenGL. SDL3 is built into the runner's
workspace because Ubuntu 24.04 predates it. macOS (`macos-15`, arm64) uses
AppleClang and Homebrew Ninja/SDL3, with deployment target 14.4; curl and
zlib come from the SDK. Windows (`windows-2022`, x64) uses clang-cl, Ninja
and the Windows SDK, with vcpkg SDL3/zlib/curl and a binary cache. Windows
10 1803 is the minimum. No packaging or installers run here. Local platform
commands are `cmake --workflow --preset macos` or `windows`; the latter
requires `VCPKG_ROOT` and an initialized Windows SDK environment.

### Temporary port gates

`cmake/Platforms.cmake` defines the boundaries. Later port steps remove
them as implementations and tests become available. Linux coverage is
unchanged. macOS and Windows currently build/test JSON/schema/params/patch,
command bus, Lua, DSP/Console 9/analysis, MIDI/notation/timeline/optimizer,
session model, plugin support, all stock SDK-free DSP engines, and the
script compiler/runtime and suite semantic layer. Audio-file tests also
run on macOS. These gates are explicit, with no no-op RT implementations:

| Gate | Off on | Reason / follow-up |
|---|---|---|
| `SUPPLE_PLATFORM_SERVICES`: scheduling, stack prefault and wake relay implementations/tests | macOS, Windows | Linux futex/eventfd and scheduling APIs; portable inline RT primitives still tested |
| Same gate: CLAP/VST3 host, scan fixtures, bridge and sandbox | macOS, Windows | Linux loader, process isolation and IPC; hosting/bridge port steps |
| Same gate: engine, JACK/fake JACK, control socket/model loop, MCP | macOS, Windows | Linux wake/process services and JACK; headless/backend port steps; session model still built |
| Same gate: DAW GUI/agent, stock plugin wrappers, conformance/reset/export integration tests | macOS, Windows | Depend on the host/engine and X11; SDK-free DSP engines and suite tests still built |
| `SUPPLE_POSIX_AUDIO_FILES`: audio-file library/tests | Windows | POSIX exclusive files, RF64 output and recording recovery; file-I/O port step |

Research GUI spikes and host fuzzers reject configuration on non-Linux
platforms rather than silently building incomplete targets. Renders must
be deterministic on each platform; cross-platform bit identity is not
required (ADR-0033).
