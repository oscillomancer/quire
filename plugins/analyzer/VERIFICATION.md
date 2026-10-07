# Gauge (analyzer): verification

The engine passes audio bit-exactly at zero latency. It copies measurement
blocks into a bounded queue read by a worker thread; hold, reset, window
changes and queue overflow never alter the audio. The editor shows
loudness, spectrum, history and stereo statistics. Scalars are read-only
parameters; the semantic telemetry is canonical JSON with the measured
analysis-report fields. Window keys, read-only ranges, silence floors,
hold/reset semantics, gating and overflow behaviour are specified in
`docs/specs/stock-effects.md` (analyzer section).

## What the tests cover

- Engine (`analyzer-engine.tests`): bit-exact output across sample rates,
  precision, channel counts and block sizes; automation and block-partition
  invariance; hold, reset, window and drop behaviour; state and telemetry
  fields.
- Plugin suite (`plugin.analyzer.ctest`): host smoke, semantic real-time
  checks and editor input.
- Monorepo: the CLAP validator, the host's compatibility with the original
  semantic-extension size, and an offline/live analysis comparison
  (`core-analysis` `LiveMeter` against the offline analyzer).

## Editor checks

```sh
build/dev/plugins/analyzer/analyzer-gui-preview 20 idle.ppm 1 idle
build/dev/plugins/analyzer/analyzer-gui-preview 20 music.ppm 1 music
build/dev/plugins/analyzer/analyzer-linux-host-probe build/dev/plugins/clap/Gauge.clap host.ppm
magick idle.ppm idle.png
```

The host probe checks native parent embedding, mapping, non-blank drawing,
resize, hide/show, three reopen cycles, timer teardown and concurrent audio.

The spectrum is of the mid signal, so anti-phase content cancels in it;
final delivery checks should use offline analysis (`skill/guides/analyzer.md`).
