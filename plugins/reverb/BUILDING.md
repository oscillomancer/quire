# Building and validation

Build from the monorepo root; dependencies are pinned under third_party and
configuration is offline. The plugin derives its artifact name from
cmake/Identity.cmake. CLAP is the default format.

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --test-dir build/dev/plugins/reverb --output-on-failure -j6
build/dev/plugins/reverb/engine/reverb-engine.tests
```

The monorepo registers the isolated template project and the SDK-free engine
separately. The artifact is build/dev/plugins/clap/Nave.clap. Engine tests
are labelled rt and run in every sanitizer preset. Product tests cover the
core, semantic edits, audio-thread tail notifications, editor input, wrapper
patches and mono/stereo CLAP host contracts. The parent also runs the pinned
CLAP validator.

The original template's standalone project presets remain available, using
the same monorepo library seam. GUI and extra wrapper formats are controlled
by REVERB_BUILD_GUI / REVERB_VST3 (AUv2 is included on macOS); those platforms require
acceptance. This worker verified Linux only. Do not infer other-platform
host behavior from a successful Linux build.

```sh
build/dev/plugins/reverb/reverb-gui-preview 0 build/dev/reverb-default.ppm 1 default
build/dev/plugins/reverb/reverb-gui-preview 0 build/dev/reverb-hall.ppm 1 hall
build/dev/plugins/reverb/reverb-linux-host-probe build/dev/plugins/clap/Nave.clap build/dev/reverb-host.ppm
```

These tools open an X11 editor and process in-memory buffers, without opening
an audio device. Preview captures prove drawing; the host probe verifies
embedding, resize, hide/show, reopen and teardown under concurrent audio.

The benchmark is deliberately outside CTest:

```sh
cmake --preset release -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG'
cmake --build --preset release --target reverb-bench -j6
build/release/plugins/reverb/engine/reverb-bench 120
```

See VERIFICATION.md for measurements, exact commands, CI results and remaining
platform acceptance. Run tools/ci/format.sh and the repository's full CI once
at the end of a task, following the worker onboarding rules.
