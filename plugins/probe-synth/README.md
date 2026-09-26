# Probe Synth

Probe Synth is Quire's internal eight-voice test instrument. It instantiates
Supple Audio's plugin template and retains its CLAP wrapper, editor window,
renderer, host probes, and gesture/telemetry infrastructure. The product-owned
DSP, parameter table, editor layout, and CLAP note/audio port handling live in
this directory. See [QUIRE.md](QUIRE.md) for the complete deviation record.

The oscillator has PolyBLEP saw and square modes, sine, and seeded noise. Each
voice has separate amp and filter ADSRs, a resonant TPT state-variable lowpass
with 12 or 24 dB slope, and oldest-voice stealing. Note on/off/choke events are
applied at their CLAP sample offsets. It has no audio input and supports mono and
stereo output configurations.

From the monorepo root, `cmake --workflow --preset dev` builds and tests it.
The CLAP binary is at `build/dev/plugins/clap/ProbeSynth.clap`. Offline DSP
render: `build/dev/plugins/probe-synth/probesynth-cli /tmp/probe.wav 60 2`.

The copied source remains MIT licensed. Bundled font and dependency notices
are in [NOTICES.txt](NOTICES.txt).
