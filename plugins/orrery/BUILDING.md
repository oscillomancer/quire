# Building Orrery

From the Quire repository root, run `cmake --workflow --preset dev`. The
ExternalProject build uses the repository's vendored CLAP SDK, clap-wrapper,
Pugl and NanoVG, with configure-time downloads disabled. It builds CLAP by
default; VST3 requires `-DORRERY_VST3=ON` and a local VST3 SDK.

Artifacts for the dev preset:

- CLAP: `build/dev/plugins/clap/Orrery.clap`
- CLI: `build/dev/plugins/orrery/orrery-cli`
- Plugin tests: `ctest --test-dir build/dev/plugins/orrery --output-on-failure`

The top-level CTest suite registers all plugin tests as
`plugin.orrery.ctest`. To render a middle-C note directly from the DSP:

```
build/dev/plugins/orrery/orrery-cli /tmp/probe.wav 60 2
```

The Linux editor embedding probe requires an X11/XWayland display:

```
build/dev/plugins/orrery/orrery-linux-host-probe \
  build/dev/plugins/clap/Orrery.clap /tmp/probe-host.ppm
```

The `semantic-conformance` and `semantic-rt` tests exercise the extension,
legacy state migration, and patch replacement during processing.
The template's `host_contract.h` checks the instrument's no-input layout,
float/double audio, non-silent note rendering and parameter text round trips.
The `rtsan` preset instruments the SDK-free DSP test.
