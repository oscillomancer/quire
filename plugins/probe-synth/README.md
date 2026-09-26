# Probe Synth

Probe Synth is Quire's internal eight-voice test instrument. It instantiates
Supple Audio's plugin template and retains its CLAP wrapper, editor window,
renderer, host probes, and gesture/telemetry infrastructure. The product-owned
DSP, semantic parameter table, editor layout, and note/audio ports live in
this directory. See [QUIRE.md](QUIRE.md) for the deviation record.

The oscillator has PolyBLEP saw and square modes, sine, and seeded noise. Each
voice has separate amp and filter ADSRs, a resonant TPT state-variable lowpass
with 12 or 24 dB slope, and oldest-voice stealing. Note on/off/choke events are
applied at their CLAP sample offsets. It has no audio input and supports mono and
stereo output configurations.

The semantic patch type is `supple.probe-synth-patch/1.0`. Parameters use
physical units in JSON; generic CLAP hosts see normalized values for
continuous parameters. For example:

```json
{"schema":"supple.probe-synth-patch/1.0","seed":42,"params":{"osc.waveform":"saw","filter.cutoff":1200,"amp.attack":25,"amp.sustain":70}}
```

From the monorepo root, `cmake --workflow --preset dev` builds and tests it.
The CLAP binary is at `build/dev/plugins/clap/ProbeSynth.clap`. Offline DSP
render: `build/dev/plugins/probe-synth/probesynth-cli /tmp/probe.wav 60 2`.

The copied source remains MIT licensed. Bundled font and dependency notices
are in [NOTICES.txt](NOTICES.txt).
