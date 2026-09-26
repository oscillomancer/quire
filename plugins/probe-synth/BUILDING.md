# Building Probe Synth

From the Quire repository root, run `cmake --workflow --preset dev`. The
ExternalProject build uses the repository's vendored CLAP SDK, clap-wrapper,
Pugl and NanoVG, with configure-time downloads disabled. It builds CLAP by
default; VST3 requires `-DPROBESYNTH_VST3=ON` and a local VST3 SDK.

Artifacts for the dev preset:

- CLAP: `build/dev/plugins/clap/ProbeSynth.clap`
- CLI: `build/dev/plugins/probe-synth/probesynth-cli`
- Plugin tests: `ctest --test-dir build/dev/plugins/probe-synth --output-on-failure`

The top-level CTest suite registers all plugin tests as
`plugin.probe-synth.ctest`. To render a middle-C note directly from the DSP:

```
build/dev/plugins/probe-synth/probesynth-cli /tmp/probe.wav 60 2
```

The Linux editor embedding probe requires an X11/XWayland display:

```
build/dev/plugins/probe-synth/probesynth-linux-host-probe \
  build/dev/plugins/clap/ProbeSynth.clap /tmp/probe-host.ppm
```

The template's `host_contract.h` checks the instrument's no-input layout,
float/double audio, non-silent note rendering and parameter text round trips.
The `rtsan` preset instruments the SDK-free DSP test.
