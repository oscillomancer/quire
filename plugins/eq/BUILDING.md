# Build and validation

Use the monorepo presets, which supply pinned permissive dependencies offline:

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --test-dir build/dev -j6 --output-on-failure
ctest --test-dir build/dev -R eq --output-on-failure
```

The CLAP artifact is in `build/dev/plugins/clap/`, named from `identity.cmake`.
`SUPPLE_PLUGIN_VST3=ON` enables the template wrapper's optional VST3 format.
Neither a build nor a preview installs into a host's scan path.

The isolated plugin tests cover host-smoke (stereo and mono), shared host
contracts, state/semantic conformance, patch replacement while processing,
phase restart/latency, editor input and wrapper patches. Monorepo
`eq-engine.tests` runs DSP, response, automation, canonical patch and state
checks with label `rt` in every preset. Run `tools/ci/run-all.sh` once for all
six presets, licensing and formatting.

```sh
build/dev/plugins/eq/eq-gui-preview 0 build/dev/eq-flat.ppm 1 flat
build/dev/plugins/eq/eq-gui-preview 0 build/dev/eq-mix.ppm 1 mix
magick build/dev/eq-flat.ppm build/dev/eq-flat.png
magick build/dev/eq-mix.ppm build/dev/eq-mix.png
```

The preview needs the workstation X11 display. It synthesizes audio through the
real processor and pre/post spectrum telemetry. See `QUIRE.md` for original
infrastructure and product deviations. Real DAW load/automation and non-Linux
platform acceptance remain separate checks for the orchestrator.
