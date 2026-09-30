# Building and validation

From the monorepo root (offline dependencies are vendored):

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --test-dir build/dev -R transient --output-on-failure
build/dev/plugins/transient/transient-gui-preview 30 build/dev/transient-default.ppm 1 default
build/dev/plugins/transient/transient-gui-preview 30 build/dev/transient-punch.ppm 1 punch
magick build/dev/transient-punch.ppm build/dev/transient-punch.png
```

The artifact is `build/dev/plugins/clap/Flint.clap`. The native Linux probe
uses a real X11 parent and synthetic audio without opening an audio device:

```sh
build/dev/plugins/transient/transient-linux-host-probe build/dev/plugins/clap/Flint.clap build/dev/transient-host.ppm
```

The CLI renders mono/stereo WAV through the same engine:

```sh
build/dev/plugins/transient/transient-cli input.wav output.wav --attack 60 --sustain -30 --mix 100 --output-gain -3 --block 257
```

`--clip` enables the 72-sample 2x soft-clip path. The renderer retains the
input file's length (leading latency included; trailing latency is truncated).
`--bits 24` chooses PCM, default 32 writes float WAV.

Engine tests carry the `rt` label. Template tests cover editor gestures,
wrapper patches, lifecycle/restart, precision, port layouts, state and
semantic patch replacement during processing. Full matrix:
`tools/ci/run-all.sh` (dev, gcc, asan, ubsan, tsan, rtsan, license, format).
Use no more than six build jobs. A native preview proves drawing; Linux
embedding, format validation and a DAW acceptance check are separate evidence.
VST3 and AUv2 require their platform toolchains and are not verified by a
Linux CLAP build.
