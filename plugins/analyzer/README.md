# Stock analyzer

A zero-latency, bit-exact monitor for mono/stereo audio. It measures live
loudness, true peak, stereo correlation/width/balance, spectral centroid and
31 third-octave bands. The display name is defined in `engine/include/analyzer/params.h`.

Build from the monorepo with `cmake --preset dev` and
`cmake --build --preset dev -j6`. The artifact is
`build/dev/plugins/clap/Analyzer.clap`. For behavior and limits see
`../../skill/guides/analyzer.md`; integration differences are in `QUIRE.md`.

Preview: `build/dev/plugins/analyzer/analyzer-gui-preview 60
build/dev/analyzer-music.ppm 1 music` (or `idle`). The real editor is rendered
via the template's Pugl/NanoVG stack. The license is MIT; third-party license
texts are staged alongside the artifact. No audio device is needed for tests.

Implementation verification and remaining platform acceptance: `VERIFICATION.md`.
