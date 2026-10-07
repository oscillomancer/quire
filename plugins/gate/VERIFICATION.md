# Sluice (gate/expander): verification

Sluice is a gate and downward expander with hysteresis and hold, dB-domain
one-pole ballistics, filtered internal or external stereo keys, stereo
linking, mix and key listen, read-only meters and a lookahead whose change
takes effect at the next restart. The lookahead opening ramp reaches unity
before the delayed transient, with no gain jump at its end. State and
controls are counted in samples, so the output does not depend on the host's
block partition. Defaults, ranges, the expander deadband, exact mute
settling, ballistics timing, latency and meter definitions are specified in
`docs/specs/stock-effects.md` (gate section).

## What the tests cover

- Engine (`gate-engine.tests`): hysteresis, hold, ballistics, expander
  ratio, range −80 dB as exact silence, lookahead alignment, linking and
  block-partition invariance.
- Plugin suite: host smoke, semantic conformance and editor input, plus a
  black-box sidechain regression: with a stereo main bus, a mono external
  key and stereo link at zero, both channels stay closed for a silent key
  and both open for a key above threshold, in float and double processing
  (a mono key once opened only the left channel).
- clap-validator 0.4.1 passes (36 passed, 8 skipped); its first run
  rejected 8 kHz, which led to the low-rate support and edge-rate delay
  tests.

## Editor checks

```sh
build/dev/plugins/gate/gate-gui-preview 20 default.ppm 1 default
build/dev/plugins/gate/gate-gui-preview 20 drums.ppm 1 drums
build/dev/plugins/gate/gate-linux-host-probe build/dev/plugins/clap/Sluice.clap host.ppm
```

The host probe checks embedding, mapping, non-blank rendering, resize,
hide/show, three reopen cycles, timer cleanup and concurrent audio.
