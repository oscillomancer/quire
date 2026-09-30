# Building and checking `comp`

Build from the repository root:

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --test-dir build/dev -R comp --output-on-failure
```

The artifact display name is set in `cmake/Identity.cmake`. The default
monorepo build publishes the CLAP under `build/dev/plugins/clap/`. VST3 is
optional (`SUPPLE_PLUGIN_VST3=ON`); macOS also builds an AU with `QCmp` subtype.
No build installs plugins automatically.

Offline editor review, with an X11 display:

```sh
build/dev/plugins/comp/comp-gui-preview 20 build/dev/comp-default.ppm 1 default
magick build/dev/comp-default.ppm build/dev/comp-default.png
```

Use `heavy` or `limiter` as the last argument for the other scenarios.
The standalone preview uses synthetic audio, without an audio device.
For an actual Linux embedding check, run `comp-linux-host-probe` with the
CLAP path and a capture PPM path. Other-platform hosts need their own checks.
