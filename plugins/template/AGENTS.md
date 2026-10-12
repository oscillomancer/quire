# The template: instructions for agents

Read [framework/AGENTS.md](framework/AGENTS.md) first. It is the contract
for every product built on this framework.

This repository is the template itself: the source of `framework/` for every
Supple Audio product, plus a starter product in `product/` that exercises it.

- **Making a new product?** Do not edit this repository. Run
  `framework/tools/new_product.cmake` (see the README) and work in the copy.
- **Fixing or extending the framework?** This is the place. Follow "Changing
  the framework" at the end of `framework/AGENTS.md`: a test that would have
  caught it, every preset green, every example green
  (`framework/tools/test_examples.cmake`), `framework/VERSION.txt` and
  `framework/CHANGES.md` updated. Then bring the change into each product
  with `framework/tools/update_framework.cmake`.
- **Adding a framework feature the starter does not use?** Grow the
  framework, not the number of frameworks: a small feature is opt-in by
  use (a parameter modifier); a larger one is a switch a product turns on
  in `product.cmake` (`PRODUCT_SIDECHAIN ON`). Either way, show it in an
  example under `examples/` -- extend an existing one where it fits, add a
  small one where none does -- and write the framework's tests against the
  product's own table, so they run for real in that example and report
  themselves skipped elsewhere.
- **Changing the starter product?** It exists to show patterns, not to ship.
  Keep it small and bit-transparent at its defaults. It uses knobs, toggles,
  a segmented control, a custom drag and a spectrum; `ui-tests` covers the
  other kinds (sliders, dropdown, buttons, XY pad, tabs, lists, keyboard,
  envelope; file slots where a product has a file property) on a synthetic
  layout, and the examples show them.

## The examples

`examples/<name>/` is a complete `product/` directory: what `new_product`
copies for `-DEXAMPLE=<name>`, and what `test_examples.cmake` generates,
builds and tests under any preset. Keep each small, measured
(`test/processor_test.cpp`), looked at (`gui-preview`; `previews.txt` names
its states) and costed (`benchmark.txt`, which `new_product` leaves behind
and `test_examples` puts back), with its own name, ID and codes, which
`new_product` then refuses to reuse.

- `instrument` (Tone): notes, voices, note ends, voice info, MIDI pitch
  bend and mod wheel, `PRODUCT_NOTE_EXPRESSIONS tuning volume`, a
  `polyModulatable()` Level, microtuning (`PRODUCT_TUNING`: MTS-ESP and CLAP
  tuning through `retuning()`), and `kOfflineMode`: bounces get a
  band-limited triangle at 20 Hz and above, measured against the live, naive
  one (below 20 Hz both use the naive shape to bound work). Built on the
  DSP library: `dsp::Voices` (stealing, the sustain pedal, Mono and Legato
  with Glide, bend) keeping the note-end books, a `dsp::Adsr` per voice, a
  polyBLEP saw, an `Lfo` for the mod wheel. An ADSR dragged on an
  `envelope()`, and an on-screen `keyboard()` that plays the processor and
  lights whatever is held.
- `pulse` (Pulse): a tremolo and auto-pan. Tempo sync (`noteValue`, the
  song position when playing), `smoothed()` parameters read from
  `Block::smoothed`, an `offAtMinimum()` level, degrees, an `output()`
  parameter shown in a readout and sent to the host, and `PRODUCT_PRESETS`:
  factory presets in categories and the preset bar. Its editor shows the
  toolkit: two `tabs()` pages, a `presetList()`, an `xyPad()` for depth and
  stereo phase, rates `enabledWhen()` sync says so, an SVG logo and icons
  (`PRODUCT_ASSETS`), and a light and a dark `Theme`.
- `duck` (Duck): a ducker keyed from `PRODUCT_SIDECHAIN`, with what it
  removes on an aux output (`PRODUCT_AUX_OUTPUTS "Ducked"`), in stereo,
  mono, 5.1, 7.1.4 and first-order ambisonics. Its header shows the track's
  name and colour, as the host gives them; `kAudioFeed` gives its editor the
  audio, drawn scrolling by with the gain reduction over it, and RMS meters.
- `chord` (Chord): a `note-effect`. Notes and MIDI in and out, no audio, a
  strum that sends notes at exact frames, every input note retired.
  Properties: a Custom chord typed as text and parsed in `property()`, and
  a `perProject()` label; presets that set them. Note names: each key is
  named by the chord it plays, renamed when Shape changes.
- `drive` (Drive): a saturator with no editor code (`PRODUCT_EDITOR_SOURCES
  ""`): the automatic editor, laid out from the parameters' groups, with
  their help as tooltips, presets and output readouts. Its shaper runs
  oversampled (`dsp::Oversampler`, Quality a `restartOnChange()` choice, the
  latency reported and the dry path delayed to match) with anti-aliased
  clipping (`dsp::Adaa`); its readouts are a true peak and short-term
  loudness (`dsp::TruePeak`, `dsp::Loudness`).
- `room` (Room): a convolution reverb whose impulse response is a file: an
  `audioFile()` property, relocated when a session has moved, reported
  missing, loaded on the framework's thread by the processor's own
  `loadFile()` -- which builds the whole `dsp::Convolution` there -- and
  swapped in by `file()`, declicked. Without one, a room of its own, built
  in `prepare()`. A `fileSlot()` in a small editor; pre-delay through a
  `dsp::FractionalDelay`.

## The starter product

A utility filter. Input Gain (dB), Low Cut and High Cut (Hz, log taper,
each with an enable toggle that crossfades over 15 ms), Slope (12 or 24 dB/oct
Butterworth, crossfaded), Mix (dry/wet, latency-compensated by `DryWet` though
the latency is zero), Output Gain after the mix, and the framework's Bypass.
No latency, no tail, no restart parameters. The editor draws the exact
response with the processor's own filter math, over the output's spectrum
(`kAudioFeed`); the cutoff handles drag through `Ui::dragEdit`; every
parameter's `help()` is its tooltip. `product/test/processor_test.cpp` measures the
Butterworth slopes, gains, mix, click-free switching and finiteness at every
sample rate.
