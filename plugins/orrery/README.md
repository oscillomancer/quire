# Orrery

Orrery is Quire's flagship synthesizer: a constrained-modular instrument
programmed through a text patch (`supple.orrery-patch/1.0`, specified in
[docs/specs/synth.md](../../docs/specs/synth.md)). A patch chooses which
modules exist — up to four oscillators (virtual analog, wavetable, FM, noise,
sub), two filters (ladder, SVF, comb, formant; serial or parallel), drive, four
envelopes, four LFOs, two step modulators, eight macros and up to five post
effects — and routes modulation as `"<source>><destination>": {"amount": …}`.

```json
{"schema": "supple.orrery-patch/1.0",
 "oscillators": {"osc1": {"type": "va", "wave": "saw"}},
 "filters": {"filter1": {"type": "ladder", "cutoff": "1.2 kHz", "resonance": "25 %"}},
 "envelopes": {"env2": {"attack": 1, "decay": "300 ms", "sustain": 0}},
 "routes": {"env2>filter1.cutoff": {"amount": "+2 oct"}}}
```

Layout:

- `engine/` — the SDK-free parameter table, patch codec and real-time engine.
- `clap/` — the CLAP core shared by every build: parameters, state, ports, the
  `com.suppleaudio.semantic-params` extension, the editor bridge and the
  structural edit helpers; also `orrery-headless.clap` for tests, the sandbox
  and the engine.
- Everything else is the plugin template copy that turns the core into
  `Orrery.clap` (and VST3 with `SUPPLE_PLUGIN_VST3=ON`) with an editor. See
  [QUIRE.md](QUIRE.md) for the deviation record.

From the monorepo root, `cmake --workflow --preset dev` builds and tests
everything; the plugin is `build/dev/plugins/clap/Orrery.clap`. The editor
preview: `build/dev/plugins/orrery/orrery-gui-preview /tmp/orrery.ppm 1 rest`.

The copied template source remains MIT licensed. Bundled font and dependency
notices are in [NOTICES.txt](NOTICES.txt).
