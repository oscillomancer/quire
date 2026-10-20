# Stock effects: implementation notes

**The parameters, names and DSP intent are in `docs/specs/stock-effects.md`**;
where a decision here changes that spec, it is written there too. This file
adds what maintainers need beyond it: the shared layout, real-time rules,
tests, GUI and skill deliverables, and per-plugin implementation notes.

Code identity uses the functional ids; each display name is defined once,
in the plugin's `cmake/Identity.cmake` (for a scripted stock plugin,
ADR-0047, in its script's `Info` and its `product.cmake`).

| id (dir, code, CLAP id suffix) | display name | CLAP id | State magic | AU subtype | Features |
|---|---|---|---|---|---|
| `yarrow` (`plugins/stock/yarrow`) | Yarrow | `com.suppleaudio.yarrow` | `LzjE` (the player's for the id) | `Yarw` | audio-effect, equalizer |
| `sundew` (`plugins/stock/sundew`) | Sundew | `com.suppleaudio.sundew` | `pqwP` (the player's for the id) | `Sndw` | audio-effect, compressor |
| `treeline` (`plugins/stock/treeline`) | Treeline | `com.suppleaudio.treeline` | `tdOY` (the player's for the id) | `Trln` | audio-effect, limiter |
| `gorse` (`plugins/stock/gorse`) | Gorse | `com.suppleaudio.gorse` | `dEQG` (the player's for the id) | `Gors` | audio-effect, transient-shaper |
| `hickory` (`plugins/stock/hickory`) | Hickory | `com.suppleaudio.hickory` | `iEKh` (the player's for the id) | `Hkry` | audio-effect, utility |
| `sorrel` (`plugins/stock/sorrel`) | Sorrel | `com.suppleaudio.sorrel` | `sTMn` (the player's for the id) | `Sorl` | audio-effect, gate |
| `teasel` (`plugins/stock/teasel`) | Teasel | `com.suppleaudio.teasel` | `BLWw` (the player's for the id) | `Tesl` | audio-effect, distortion |
| `analyzer` | Gauge | `com.suppleaudio.analyzer` | `QAN1` | `QAna` | audio-effect, analyzer, stereo |
| `delay` | Echelon | `com.suppleaudio.delay` | `QDL1` | `QDly` | audio-effect, delay, stereo |
| `reverb` | Nave | `com.suppleaudio.reverb` | `QRV1` | `QRvb` | audio-effect, reverb, stereo |

In the template's identity checklist the code name is the id
(`Analyzer`/`analyzer`/`ANALYZER`); only the user-visible name string is the
working name, defined once per plugin.

## 1. The shape every effect shares

### 1.1 Layout

Each stock effect defines its display name only in
`plugins/<id>/cmake/Identity.cmake`: `set(<PREFIX>_DISPLAY_NAME "<Name>")`.
The engine receives `SUPPLE_<PREFIX>_DISPLAY_NAME` as a public compile
definition; its name constant and the plugin's artifact/bundle/test paths
use that value. `supple_add_plugin(<id> <PREFIX>)` reads the identity file
itself; an optional third argument names artifacts for template products
without that file. CLAP ids, state magics and functional code identities
stay fixed when the display name changes.

Like Orrery (`plugins/orrery`):

- `plugins/<id>/engine/` — the SDK-free core, a monorepo library
  `supple::<id>-engine` (namespace `supple::<id>`), built and tested by the
  monorepo with its warnings and all sanitizers (label `rt` on its tests):
  - `include/<id>/params.h` — the one `core-params` table (`ParamSpec`s:
    permanent CLAP ids, keys, units, ranges, curves, tags, descriptions) and
    the `patch::FlatPatchType` (`"supple.<id>-patch/1.0"`, no seed);
  - `include/<id>/processor.h` + `src/` — the DSP: `prepare(sampleRate,
    maxFrames)`, `reset()`, `setParam(index, physical)` (RT), `process(in, out,
    frames[, key])` (RT, `SUPPLE_RT`), `latency()`, plus pure functions the
    GUI and tests use (e.g. the EQ's magnitude response);
  - `describe()` — the compact prose (§1.6);
  - `tests/` — the monorepo tests of §1.7.
- `plugins/<id>/` — a copy of the template made exactly as
  `plugins/probe-synth` was (read its `QUIRE.md`: identity renames, vendored
  dependency paths, `supple_plugin_add_monorepo_libraries()`, format-gate and
  license-check handling, `supple_add_plugin(<id> <PREFIX>)` in
  `plugins/CMakeLists.txt`). The template is an effect already: keep its
  audio-port and process structure. Its `*_clap.cpp` product sections forward
  to `supple::clapx::ParamStore` / `ClapParams` / `FlatSemantic`
  (`plugins/common`) and to the engine; `QUIRE.md` lists every deviation.
  The shared template infrastructure stays byte-identical apart from names.
- The template's DSP header (`dsp/include/<id>/Processor.h`) is replaced by
  the engine library, as in Orrery.

A change to one plugin stays in its directory. DSP primitives that two
plugins need move to `libs/core-dsp` (the shared oversampler is an
example), with their tests.

### 1.2 Parameters and state

- Keys are exactly `stock-effects.md`'s (flat snake case, `bandN.` for EQ
  bands); groups follow the spec's grouping. Units are canonical (`Hz`, `dB`, `ms`, `percent`,
  `ratio`); curves per ADR-0010 (log for frequencies and times). Every float
  gets a `smoothingMs` (20 unless stated) and tags from the role vocabulary.
- Read-only meters are `kReadonly` output parameters (as ProbeSynth's
  `output.level`), updated from the audio thread through the `ParamStore`
  and reported to the host with `CLAP_EVENT_PARAM_VALUE` output events at most
  every 50 ms.
- Patch and state: `FlatSemantic` (framed canonical JSON, transactional
  load); the extension's `describe` returns §1.6's prose.
- The table passes `params::validateTable` in a test; ids are dense from 0 in
  table order and never renumbered afterwards (append only).

### 1.3 Real-time rules

- `process` never allocates, locks, logs or waits; everything is sized in
  `prepare` (max block and the largest sample rate's delay lines).
- **Block-size invariance:** for the same input and the same parameter events
  (by sample offset), the output is bit-identical whatever the host's block
  partition (1, 7, 64, 4096 frames). Smoothing and coefficient updates run per
  sample (or on a fixed internal grid of 16 samples counted from `reset`, never
  per host block).
- Any sample rate 22 050–384 000 Hz; frequencies above 0.45·fs are clamped
  internally (the parameter keeps its value).
- EQ replaces non-finite input samples with zero before recursive/FIR state.
  Hickory (the utility) makes its working values finite before any filter
  or delay state, while at rest its output keeps the input's bits.
- Denormals: `dsp::DenormalGuard` around processing; recursive states are
  also flushed to zero below 1e-30 when the input has been silent.
- Parameter changes are smoothed (no zipper noise); a switch that cannot be
  smoothed (a filter type, a detector mode) crossfades over 10 ms.
- Mono (1-in/1-out) works where the template supports it; stereo is the
  default configuration.
- Latency is reported through `clap.latency`. Latency-changing parameters
  (lookahead, linear phase) are stepped and not automatable
  (`stock-effects.md` §0) and follow the template's restart contract.

### 1.4 Determinism

No randomness. The same state, input and events give the same output across
runs, processes and sample-rate-independent block partitions (§1.3).

### 1.5 GUI

Product-owned `Editor.cpp`/`Theme.h`/`Widgets.h` usage (the template's widgets
and window stay as shipped). Each plugin's layout is in its section below;
all read names, units and formatted values from the table, and every edit is
a gesture-bracketed parameter change.
`tools/gui_preview` renders at least two scenarios per plugin to PNG for review.
Design size ≤ 900 × 560. The look follows the product theme used by Orrery
(`plugins/orrery/plugin/gui/Theme.h`): same palette and type, so the stock
plugins read as one family. The scripted stock plugins that replace them
(ADR-0047) take the scripts' warm palette instead (`stock-effects.md` §0).

### 1.6 describe()

At most 300 characters for `changedOnly`, listing only what is not at its
default, in signal-flow order, with units — e.g. the EQ: `high-pass 80 Hz
24 dB/oct → bell 250 Hz −3 dB Q 1.2 (mid) → high shelf 10 kHz +2 dB; output
−1 dB`. Full detail may list everything.

### 1.7 Tests

In the engine library (monorepo, all presets):

- table validation, patch round trip (defaults and a full non-default patch),
  merge errors with paths;
- **null test:** default parameters pass audio through bit-exactly (EQ with
  every band flat or off, utility, analyzer) or within the plugin's stated
  tolerance (compressor below threshold: unchanged within 1e-6, delayed by
  its latency);
- **block-size invariance** (§1.3) with automation events;
- **sample-rate sweep** 44.1/48/88.2/96/192 kHz: the documented response
  holds at every rate (per-plugin checks below);
- **automation stress:** every parameter jumping to random values every
  1–64 samples for 5 s of noise: finite, bounded output (no NaN/Inf, peak
  below +40 dBFS), no denormal stalls;
- **state:** set random values, save, load into a fresh instance, identical
  output;
- the per-plugin behaviour tests below, each with a measured tolerance.

In the plugin copy (as ProbeSynth): `host_smoke`, `semantic_rt`,
`editor_input`, the template's host contract, and the clap-validator run
(`plugin.<id>.validator`, label `slow`).

### 1.8 Skill and docs

- `skill/guides/<id>.md`: what the plugin is for, every key with its unit
  and range, three short recipes (e.g. the EQ: remove rumble, carve a
  masking region reported by `analyze.masking`, de-harsh), and honest limits.
  Linked from `skill/SKILL.md` and `skill/guides/mixing.md`.
- `plugins/<id>/QUIRE.md` (deviations from the template) and a STATUS row.

## 2. EQ — Yarrow [`yarrow`] (`stock-effects.md` §1)

A DSP script (ADR-0047), `plugins/stock/yarrow/script/yarrow.cpp`; the
spec section holds the designs and measured accuracy. Notes for maintainers:

- **Audio.** `update()` reads the parameters into eight `Setting`s and, when
  auto gain is on, works out its offset; `control()` runs every 16 samples
  (a counter kept across blocks) and takes structure changes once a band
  has faded out, glides frequency, gain and Q, follows dynamic bands and
  redesigns a band only when its values moved; `process()` runs each band
  that is on or fading, in index order, placing it on both channels, the
  mid or side (encoded around that band only) or one channel, then listen,
  then the output gain. The first `update()` after `prepare()` (or a reset)
  snaps every glide, so a render after `reset()` matches a fresh instance.
  Non-finite input is zeroed before any band.
- **Editor.** Its own members only: the band settings read from the
  parameters each repaint, each band's sections designed once per repaint,
  the transform's tables and buffers, and the pointer's state. The audio
  code shares the output ring (`SharedArray<float, 32768>`), its head and a
  running sample count (the analyser's clock), peaks, each band's live gain
  and detector level, and the auto-gain offset. Edits go through
  `change()`/`act()`, which select a band's parameter by index.
- **Tests.** `plugin.yarrow.response` measures bells, cuts and shelves
  through the built plugin; `plugin.yarrow.catalog` keeps `Catalog.cpp` in
  step; the template's suite, reset, unload, semantic conformance and
  clap-validator checks run as for every stock plugin. During development,
  `dsp.test` with `automation` (no excess step on type, channel or enable
  switches) and `dsp.preview` with `pointer` steps covered clicks and every
  gesture.

## 3. Compressor — Sundew [`sundew`] (`stock-effects.md` §2)

A DSP script (ADR-0047), `plugins/stock/sundew/script/sundew.cpp`; the
spec section holds the laws and ballistics. Notes for maintainers:

- **Audio.** `Law` holds a character's static law and its one-pole steps at
  one sample rate (`set()` in `update()`, again when the tempo moves a synced
  release); `Cell` is one channel's gain cell, stepped once per sample with
  that channel's detector level. The feedback cells (Opto, FET) take the
  key's level before their own gain and solve the implicit one-pole with
  three Newton steps, so the loop is stable at any attack and ratio.
  `process()` per sample: key (input or connected sidechain, high-pass,
  mid/side) → level (peak or mean square, then link) → cells → gain on the
  lookahead-delayed audio → colour → mix → match → output → listen. A change
  of character settles the new cells on the old amount, or (to or from
  Bloom) at rest with `bridge`, a 30 ms glide of the old gain. `unplayed`
  zeroes the match until audio plays, so a render depends only on its values.
- **Lookahead** is a `.restarts()` choice: `prepare()` reads it and reports
  the latency, so a change waits for the host's restart, and the delay in use
  is always the one reported. The key is never delayed (the detector looks
  ahead); listening to the key reads it through its own delay.
- **Editor.** Its own members only: a `Law` and a `Cell` for the
  ballistics plot and the character icons (computed once, and again only
  when a value the plot depends on moves), the needle's motion and peak, the
  dot's trail and the pointer's state. The audio code shares the history
  (512 columns of 10 ms: input and compressed peaks, gain change, bar or
  beat), the key level, the gain change, peaks, the match and loudness, the
  tempo, the bar length and whether the external key is live.
- **Tests.** The template's suite, reset (also `--stepped`), unload,
  semantic conformance, clap-validator and the catalog check run as for
  every stock plugin. During development, `dsp.test` measured the static
  law of every character (within 0.3 dB of the set ratio at 6 and 24 dB
  over), external-key ducking (−21.4 dB for −21.6 expected), the latency
  of each lookahead, onset peaks with and without lookahead, the match
  converging on the dry loudness, and no excess step when switching
  character (including to and from Bloom), threshold, mix, output, stereo,
  key filter, match, colour and sync; `dsp.preview` with `pointer` steps
  covered every gesture.

## 12. Limiter — Treeline [`treeline`] (`stock-effects.md` §10)

A DSP script (ADR-0047), `plugins/stock/treeline/script/treeline.cpp`; the
spec section holds the detector, the gain path and the loudness meters.
Notes for maintainers:

- **Audio.** `TruePeak` is one channel's detector (the phases are designed
  in `prepare()`); `Hold` the monotonic queue; `Box` a running sum that is
  summed afresh every 65536 samples so rounding never builds up; `Bell`
  three boxes in a row. `process()` per sample: drive → detector →
  requirement → `listenTo()` (sharpness and the bass guard) → hold →
  two-stage release → both bells and their blend → the delayed audio →
  the output's own detector and K-weighting → listen. The guarantee rests
  on each bell's span being no longer than the hold and on the delay
  matching the broad bell's span: keep them together when tuning.
- **No second stage.** A gain moving within a millisecond widens the
  signal's spectrum, so in principle the product can pass the ceiling
  between samples. A second, 0.5 ms stage on the output was tried: it
  never took more than 0.001 dB on any test material, while costing a third
  of the CPU and a millisecond of latency, so it went. What decided the
  accuracy was the interpolator's length (32 taps per phase read up to
  0.5 dB low on noise-like hits; 48 do not).
- **Loudness.** `closeBlock()` every 100 ms: the 30-block ring and the
  300-block recent rings (momentary energies, short-term values, true
  peaks), then either the recent readings (two passes over the ring for
  the integrated gate; the LRA's gated values insertion-sorted, at most
  300) or, while accumulating, the integrated histogram (gate recomputed
  from bin sums: no block list grows) and the LRA histogram.
  `restartLoudness()` on the transport's start; `restartTotals()` when
  `accumulate` changes (in `update()`).
- **Editor.** Its own members only: eased values for the engine plots (the
  first frame starts where the audio is, so a preview shows the moment),
  the gain bar's held peak and the pointer's state. The audio code shares
  the history (1024 columns of 10 ms: input and output envelopes, the
  reduction and its sustained part, the momentary loudness, bar or beat) and
  the engine's and meters' values. The gain-to-target button edits `gain`
  as a click on a knob would.
- **Tests.** The template's suite, reset (also `--stepped`), unload,
  semantic conformance, clap-validator and the catalog check run as for
  every stock plugin; `plugin.treeline.response` drives a sine, a sweep,
  noise and an impulse 12 dB into a −3 dB ceiling at 44.1, 48 and 96 kHz
  (each peaks at −3.0) and checks a sine under the ceiling passes
  unchanged. At 88.2 and 192 kHz too the ceiling held; latency is 199,
  215, 376, 407 and 791 samples at 44.1, 48, 88.2, 96 and 192 kHz. During development, on
  synthetic club, acoustic, dense and true-peak material driven 12 dB:
  the output's true peak, read offline with the same detector, stayed
  within 0.001 dB of the ceiling; against BS.1770's example meter within
  that meter's own +0.17 dB passband ripple; an ideal (24 kHz brickwall)
  reconstruction reads up to 0.5 dB higher only on material with strong
  content above 21 kHz (clipped noise). A 50 Hz sine under 7 dB of
  reduction at a 20 ms release had 0.0003 % THD with the bass guard and
  0.0055 % without. CPU: about 2 % of a core in a release build at 48 kHz
  stereo (four interpolators).

## 9. Transient shaper — Gorse [`gorse`] (`stock-effects.md` §4)

A DSP script (ADR-0047), `plugins/stock/gorse/script/gorse.cpp`; the spec
section holds the detector, the law and the hit view. It keeps Flint's
design (a held peak, fast and slow followers in dB, one part at a time) and
changes what Flint got wrong in use. Notes for maintainers:

- **The hold.** Flint measured a cycle between any two local maxima. On
  real material, noise or a quantized waveform's ripple near its crest
  makes maxima every few samples, so its hold sat at 1 ms and a bass line
  or a kick's body rode through the detector as onsets: on 16-bit drums a
  kick's tail read as onsets every cycle (sustain −6 dB gave −0.5 dB). The
  hold now counts crests only across a real dip, follows a leading low band
  for a whole cycle (`listenLow()`), and is never shorter than `length`
  (a voice-like 180 Hz pulse took +11.9 dB of attack at +12 before that,
  0.0 after).
- **The law.** A fall counts three times as readily as a rise (`kTail`),
  since a decay's lead is a few dB where an onset's is tens: on a kick
  decaying 78 dB/s, sustain −6 dB now gives about −4 dB at default
  settings. The slow follower stays within 30 dB of the fast one
  (`kSpread`). To watch the law at work, have a scratch copy
  (`plugins/stock/README.md`) write o and fast − slow to its output in
  place of the audio, and render material through it.
- **Lookahead.** Flint's 1 ms one-pole let the first millisecond through:
  an attack cut missed the click it was meant to soften. Two boxes ramp the
  gain over the 1 ms lookahead and land on the onset. `low` focus hears
  through about 2 ms of filter delay, so its gain lands about a millisecond
  after the onset; it is for the kick's body, not its click.
- **The clip** moved to the end (after mix and output), where its
  minimum-phase oversampler cannot comb against the dry path, and runs only
  while it is on or fading. Its output is clamped to ±1: on a snare driven
  24 dB over, the decimator rang to +7.5 dBFS.
- **Hits.** `startHit()` on a trigger (closing the hit being captured),
  `captureStep()` per sample on the delayed input and the mixed output,
  `closeBin()` per point (a point shorter than a sample repeats the last),
  `finishHit()` into the average (a capture under 10 ms is a double
  trigger and is dropped), `measureHits()` for the meters. The arrays reach
  the editor only when a hit finished.
- **Editor.** Its own: the model hit (`refreshModel()`, recomputed when
  attack, sustain, length or sensitivity change; 16 kHz steps for 501 ms),
  the eased points it draws, and the pointer's state. The colour of a
  point is the sign of the mean o over it.
- **Tests.** The suite every stock plugin has (reset, also `--stepped`;
  unload; semantic conformance; clap-validator; catalog), and
  `plugin.gorse.response`: a sine starting from silence is a hit with no
  tail; with attack +6 its onset peaks exactly 6 dB higher at −12 dBFS
  (44.1, 48, 96 kHz) and at −32 dBFS, its steady part passes at 0.0 dB and
  0.0 % THD; driven 24 dB into the clip, a sine, a sweep, noise and an
  impulse peak at 0.0 dBFS. During development on synthetic drums (kick,
  snare, hats at 120 BPM), attack ±6 changed every hit's first 2 ms by
  ±6.00 dB, at −6 and at −26 dBFS alike, and defaults passed bit-exactly.

## 4. Utility — Hickory [`hickory`] (`stock-effects.md` §8)

A DSP script (ADR-0047), `plugins/stock/hickory/script/hickory.cpp`. It
keeps Plumb's controls and laws (the linear balance, bass mono as a
Linkwitz–Riley high-pass on the side only, bit-exact at rest) and changes
the rest. Notes for maintainers:

- **Order.** Polarity → channels → DC block → delay → width and bass mono
  (M/S) → balance → gain and mute. Polarity comes first (Plumb flipped
  after the routing), so `mono` with `invert_right` sums a flipped pair
  correctly. Swap is a `channels` option rather than a switch of its own,
  which removes Plumb's redundant combinations (swap with left-to-both).
- **At rest.** Each stage that is at rest leaves the sample alone, and a
  sample no stage touched is not written back: the output is the input's
  bits, non-finite values included. Working values are made finite before
  any filter or the delay line sees them, and those filters run all the
  time, so switching the DC block or bass mono on fades in settled state.
- **Ramps.** The routing matrix, polarity, the DC block, bass mono and
  mute crossfade over 10 ms (`Ramp`, which jumps while unplayed); gain,
  width and balance glide over 20 ms (`.smoothed()`). A new delay time
  crossfades from the old tap over 20 ms, and a change that arrives during
  a crossfade waits for it to end, so automation never jumps.
- **Delay** is whole samples (`roundToInt`), so a delayed signal is an
  exact copy; it is not reported as latency, since lining a track up
  against another is the point.
- **DC block** is a 12 dB/oct Butterworth high-pass at 5 Hz (Plumb's was a
  one-pole): 0.02 dB down at 20 Hz.
- **Readouts.** 100 ms chunks of LL, RR, LR, MM, SS and the peak, summed
  over the last 30 (`closeChunk()`): `output_peak`, `correlation` (0 in
  silence), `level_balance` (R over L) and `side_level` (S over M, −60 when
  mono).
- **Editor.** The image is 1024 points of (M, S), one every 1/12000 s,
  scaled so their spread fills a third of the circle (and their peaks no
  more than twice it). Width by frequency is ten octave band-passes
  (Q 1.414, 31 Hz to 16 kHz) on each output channel, their correlation
  over 250 ms; an octave fades with its level. A press there moves the
  crossover and turns bass mono on; a horizontal drag across the image
  changes the width. The warning under the image shows while the 300 ms
  correlation is below −0.3.
- **Tests.** The suite every stock plugin has (reset, also `--stepped`,
  which leaves alone a switch that silences the plugin by itself, here
  `mute`; unload; semantic conformance; clap-validator; catalog), and
  `plugin.hickory.response`: a sine passes at 0.00 dB, gain −6 and +12.5
  measure exactly, gain −60 (−inf), mute and `side` of identical channels
  are silent, mono with width 0 and bass mono leaves identical channels at
  0.00 dB, and the DC block takes 0.02 dB at 20 Hz. During development on
  stereo test tones, bass mono at 150 Hz lowered the side at 50 Hz by
  38.3 dB and left it at 1 kHz unchanged, with the mono sum unchanged to
  float rounding; width 0 and 200, every routing, the flips and a 10 ms
  delay (an exact shift) measured as specified, and the readouts read
  correlation −0.71 on a flipped pair, a −6.02 dB balance where the right
  was 6 dB down, and −60 dB of side at width 0.

## 5. Analyzer — Gauge [`analyzer`] (`stock-effects.md` §9)

- Passes audio bit-exactly (tested at every block size and rate).
- Measures with `core-analysis` streaming pieces (the `LoudnessMeter` for
  M/S/I, LRA, true peak; correlation, width, balance; a smoothed 1/3-octave
  spectrum and the spectral centroid) on a worker path that never blocks the
  audio thread: the audio thread only copies blocks into a preallocated
  SPSC ring; measurement runs on the main thread's timer (or, if a
  measurement is RT-safe and cheap per sample, on the audio thread itself —
  justify in the report). If `core-analysis` lacks a streaming piece you
  need, add it there in the same style with tests (this plugin may touch
  `libs/core-analysis`; no other worker does).
- Readonly CLAP parameters for the scalar values (integrated, short-term,
  momentary LUFS, LRA, true peak, correlation, width, balance, centroid);
  `window`, `reset` (momentary: a host write of 1 resets and the parameter
  returns to 0), `hold`.
- **The extension addition** `get_telemetry(plugin, out)`: appended to
  `clap_plugin_supple_semantic_params_t` per the extension's versioning rule
  (`docs/specs/semantic-params-extension.md`), writing
  `supple.analysis-report/1.0`-shaped JSON of the current window. Update that
  spec, `plugins/common` (the struct and a `FlatSemantic` hook), and keep
  every existing plugin building (a null member for plugins without
  telemetry). This plugin may touch `plugins/common` and that spec; no other
  worker does.
- Tests: bit-exact pass-through; on a −23 LUFS EBU test signal (generate a
  1 kHz sine at the level core-analysis's own tests use) the reported
  integrated loudness is within 0.1 LU of core-analysis's offline result for
  the same audio; true peak and correlation likewise; the telemetry JSON
  validates against the analysis-report shape.
- GUI: loudness numbers large (M/S/I, LRA, TP), a short-term history graph,
  the 1/3-octave spectrum, correlation and balance meters. Scenarios:
  `idle`, `music` (a synthetic mix fed through the telemetry path).

## 7. Saturation — Teasel [`teasel`] (`stock-effects.md` §5)

A DSP script (ADR-0047), `plugins/stock/teasel/script/teasel.cpp`. It
replaced Kiln (`com.suppleaudio.drive`) with a new design. Notes for
maintainers:

- **Everything oversampled.** The band split, the dry signal and the clean
  bands run at the high rate with the curve, so mixing and splitting
  never comb against the oversampling filters. `phase` picks the script
  runtime's minimum-phase half-bands or its linear-phase ones (a restart);
  at 1x the oversampler is prepared at a factor of 1 and passes samples
  through (before the runtime took 1, a restart from 4x to 1x went on at
  4x). Oversampling stops at 768 kHz inside.
- **Filters about to come in run beforehand.** A low-pass switched in
  from a cold state (the high cut, the band's upper crossover) outputs
  nothing at first, a click; each runs while its parameter's target is
  inside its range. Whether they run is decided each frame, never each
  block, so renders do not depend on block size (the feedback loop's too).
- **Rest offset.** Each curve's output less f(bias) (its value with no
  signal), at once: a bias otherwise steps from 0 on the first sample and
  rings for seconds in the feedback loop. Tape seeds its direction memory
  from the first sample for the same reason.
- **Feedback** is scaled by 1 / the curve's steepest slope over ±1 about
  the bias (fuzz has a gain of 4; crush's steps count as tanh's), so the
  loop's gain stays under 1, and lets go over 200 ms once the input has
  been digitally silent for a second: without it, fuzz held a ring at
  −13 dB and crush a quantisation limit cycle at −62 dB forever.
- **Auto gain** is a table over drive, rebuilt in `update()` when style,
  character or bias change, read per sample at the modulated drive; it
  holds the reference level, not the program's (no level riding).
- **Readouts.** `thd` and `even` come from a probe (a 100 Hz sine through
  a curve of its own at the base rate, harmonics by correlation over 20
  cycles); `distortion` from running moments of the curve's input and
  output. Both are meters, never audio.
- **Tests.** The suite every stock plugin has, and
  `plugin.teasel.response`: dry (mix 0) a −12 dBFS sine passes at
  0.00 dB, and −6.00 with output −6; `soft` at −12 dB with auto gain
  keeps 0.00; `clip` without auto gain below its ceiling is the drive
  (+6.00); driving only the side leaves a centred sine at 0.00, and a band
  above 2 kHz leaves 100 Hz at 0.00 under 24 dB of drive; dry at 8x with
  linear phase a 10 kHz sine passes at 0.00; and the `thd` readout is
  within 0.1 % of `dsp.test`'s own THD (its `meters`) for tape at 6 dB and
  tube at 12 dB with bias. Host-smoke's
  latency check (quality) caught the 1x restart above. Measurements are in
  `stock-effects.md` §5. Release build, one core, stereo 48 kHz, pink
  noise: 1.9 % at the defaults (4x), 0.9 % at 1x, 3.0 % at 8x, 5.0 % at
  16x; 4.4 % with band, feedback, dynamics, motion, emphasis and a cut on,
  13.8 % with all of that at 16x.

## 8. Gate / expander — Sorrel [`sorrel`] (`stock-effects.md` §3)

A DSP script (ADR-0047), `plugins/stock/sorrel/script/sorrel.cpp`. It
replaced Sluice (`com.suppleaudio.gate`) with a new design. Notes for
maintainers:

- **Two clocks.** The detector and the state run on the key as it
  arrives, a lookahead ahead of the audio. Each sample's target gain goes
  into a line (stored 200 dB up, so the line's first zeros read as closed);
  the gain the audio now playing gets is the largest target of the next
  `attack` + 1 ms (a monotonic-queue peak hold, at most the lookahead) for
  rising, and the current one for falling. That is what lands the opening
  a millisecond before the threshold crossing and starts the fall exactly
  where the state closed.
- **Shapes.** The gate opens along a raised cosine in amplitude (its
  largest step at 1 ms is 0.033 of full scale per sample at 48 kHz);
  Expand rises along a one-pole in amplitude; both fall at a steady rate
  in dB, its speed eased in over 3 ms. A gain the closed range asks for is
  never passed under, and −80 dB (the range's minimum) is −90 dB and then
  zero.
- **Follow** reads the detector's level once a millisecond from a ring of
  2048. Measuring only up to the close crossing reads an abrupt stop as no
  fall at all (the level is still high there), hence the 20 ms after it.
- **Suggestion.** Tried on drum spill, a ringing tom, a sung line over
  hiss, tone bursts and a bass note: splitting the histogram in two
  (Otsu) split a soft passage from a loud one as readily as noise from
  sound, and walking up from the noise's peak to where the counts fall
  found no edge, since decays fill the levels above the noise. Measuring
  the noise's spread from below did not fail on any of them (snare spill
  −39.5, tom −35.5, vocal −34.5, bursts over −70 dBFS noise −48.5, bass
  −43.5 dBFS, each 12 to 14 dB over its floor).
- **Lookahead** is a choice (`off`, 1, 2, 5 ms) with `.restarts()`;
  latency is the lookahead in samples at the running rate.
- **Editor.** The history's out trace is what the gate passes, whatever
  listen plays. The SET pill takes `suggested_threshold`; the gain lane
  maps 0 to −80 dB and fills from the range up.
- **Tests.** The suite every stock plugin has, and
  `plugin.sorrel.response`: a −12 dBFS sine passes at 0.00 dB with and
  without lookahead; under a −6 dB threshold it holds at −40 dB, and at
  range −80 it is silent; Expand at 2:1 measures −6 dB; a 2 kHz key
  high-pass keeps a 100 Hz sine from opening it. The reset check's
  stimulus ends in a quiet quarter so that its S1 (threshold −60) and
  defaults differ for a gate. During development, on the material above:
  every hard onset passed bit-exact through its first 3 ms, and a ghost
  note's rising noise burst no longer lost 4 dB at its onset; with the
  default range the gaps were 40 dB down; the 40 Hz note, a tone wobbling
  ±2 dB around the threshold and the vocal phrases each opened it once;
  Follow closed a tom ringing at 30 dB/s at about 120 dB/s (the clamp) where
  the fixed release fell at 400 dB/s; about 2 % of a core in the player.

## 10. Third wave: delay — Echelon [`delay`] (`stock-effects.md` §6)

Identity: display name Echelon, id `delay`, CLAP id `com.suppleaudio.delay`,
state magic `QDL1`, AU subtype `QDly`, features audio-effect, delay,
stereo. Everything in §1 applies; no sidechain port; latency 0.

- **`sync`** keys and labels exactly as Orrery's note values
  (`plugins/orrery/engine/src/params.cpp` `kNotes`: `off`, `n8_1` … `n1_32`,
  labels `8/1` … `1/32`; default `n1_8d` = "1/8."). Copy the table, do not
  link Orrery. Tempo comes from the CLAP process transport
  (`CLAP_TRANSPORT_HAS_TEMPO`), 120 BPM without one. A synced time is the
  note length at the current tempo; every line time (synced or `time`, and
  the offset right time `time·(1 + offset/100)`) is clamped to 1..5000 ms —
  document the clamp.
- **Lines:** one circular buffer per line (float storage), sized in
  `prepare` for 5000 ms + the maximum modulation + interpolation margin at
  the prepared rate. Fractional reads with 4-point cubic Hermite
  interpolation.
- **Glide:** each line's delay (in samples) follows its target through a
  critically damped two-pole smoother (time constant 100 ms), so time and
  tempo changes move the read head continuously (tape-like pitch bend, no
  clicks, no crossfades). The same smoother serves sync and free time.
- **Wow (`mod_rate`, `mod_depth`):** delay offset = depth·3 ms·(0.8·sin +
  0.2·seeded smooth random), right line's sine at +90°; capped at 45 % of
  the line's current delay; `mod_depth` 0 is exactly no modulation. The
  random part is seeded at `reset()` (deterministic renders).
- **Loop, per line:** y = read → high-pass then low-pass (12 dB/oct TPT
  SVFs, Butterworth Q; `highpass`/`lowpass` smoothed) → `drive` (at 0 dB
  bypassed exactly; above, tanh(g·y)/g with g from `drive`, i.e. level-
  preserving for small signals) → safety limiter → this is the echo (wet)
  and, × `feedback`, what is written back with the input. The safety
  limiter is linear below 0.5 and soft-saturates to ±1.0 above (document
  the curve), so feedback > 100 % self-oscillates up to ≈ 0 dBFS and never
  beyond, and nothing below −6 dBFS is touched.
- **Modes:** `stereo` — two independent lines; `ping_pong` — the mono sum
  (L+R)/2 enters the left line only; left line input = in + fb·R_echo,
  right line input = fb·L_echo (first echo left, then alternating);
  `mono` — the mono sum through the left line, echo to both outputs.
- **`width`:** mid/side on the echo only (S × width/100); `mono` ignores it.
- **`duck`:** detector = input peak max(|L|,|R|) with 1 ms attack, 250 ms
  release, in dB; echo attenuation = `duck`·clamp((level + 60)/30, 0, 1) dB
  (none at −60 dBFS and below, the full `duck` at −30 dBFS and above).
- **`freeze`:** input to the lines fades out over 10 ms; the loop moves
  (10 ms crossfade) to exact recirculation (gain 1, filters, drive and
  limiter out of the loop), modulation fades to 0, line times hold their
  current values until unfrozen. Off returns to the normal loop over 10 ms.
- **`mix`/`output`:** linear dry/wet crossfade (as the other stock
  effects), then output gain; both smoothed.
- **Tail:** add the CLAP tail extension to this plugin (the template has
  none; the host already queries it): infinite while `freeze` is on or
  `feedback` ≥ 100 %, otherwise the longest line time ×
  ceil(ln(1e-4)/ln(feedback)) + 50 ms (at least one repeat). Tell the host
  (`clap_host_tail::changed`, from the main thread via `request_callback`)
  when it changes.
- Tests (beyond §1.7): impulse → first echo at the line time (fractional,
  within one sample, measured by cross-correlation) and each repeat × fb
  (within 0.2 dB at filters wide open); sync at 120 BPM, 1/8. = 375 ms, and a
  transport tempo change moves the echo with the glide; a time jump
  (375 → 200 ms) on a sine gives no click (the second difference of the
  output stays within 4× its steady-state peak); ping-pong alternates left
  and right with the offset spacing; feedback 110 % after a noise burst
  stays ≤ 0 dBFS peak for 30 s, keeps oscillating (last-second RMS above
  −30 dBFS) and never produces NaN/Inf; freeze holds the loop RMS within
  0.2 dB over 20 s and ignores new input; duck 12 dB with a steady −20 dBFS
  input attenuates the echo by 12 ± 0.5 dB and recovers after the input
  stops; `mono` gives L = R; width 0 gives echo L = R; tail values.
- GUI: the echo pattern (left and right tap positions and levels for the
  first repeats, drawn from the parameters and tempo), time/sync and
  feedback large, filters as a small loop-response curve, mod, drive, duck,
  width, freeze, mode, mix, output. Scenarios: `default`, `pingpong` (1/4,
  `ping_pong`, feedback 55, mod_depth 30).

## 11. Third wave: reverb — Nave [`reverb`] (`stock-effects.md` §7)

Identity: display name Nave, id `reverb`, CLAP id `com.suppleaudio.reverb`,
state magic `QRV1`, AU subtype `QRvb`, features audio-effect, reverb,
stereo. Everything in §1 applies; no sidechain port; latency 0.

An original design; write its structure and constants into
`stock-effects.md` §7 as you settle them.

- **`early`** dB uses the utility's gain convention: a finite −60 dB endpoint
  with `kOffAtMin` (exactly off, displayed −inf), up to +6.
- **Input:** the stereo input → `low_cut`/`high_cut` (12 dB/oct) →
  `predelay` (fractional, glides with a 50 ms two-pole smoother like the
  delay's) → early reflections and the diffuser.
- **Early reflections:** a tapped delay per channel, 12 taps in 5..80 ms
  scaled by size, gains falling with time, seeded tap times (fixed seed, so
  every instance and render is identical), alternating pan; level `early`.
- **Diffusion:** 4 series Schroeder allpasses per channel (1..12 ms scaled
  by size, seeded), coefficient 0.7·diffusion/100, capped so diffuser RT60
  stays below half the shortest requested band tail (stock-effects §7).
- **FDN:** 16 lines, base lengths spaced geometrically over ≈ 25..95 ms with
  seeded ±5 % jitter, all × a size factor 2^((size − 50)/50·1.5)
  (≈ 0.35×..2.8×). Buffers sized in `prepare` for the largest size plus
  modulation; size changes glide the fractional read positions (twelve-point
  Lagrange FDN reads; cubic Hermite for early/diffuser reads). Mixing: the
  normalized 16×16 Hadamard matrix as a fast
  Walsh–Hadamard transform (×1/4). Input injection and the stereo output
  taps use fixed orthogonal ± sign vectors (output scaled 1/√8).
- **Decay:** per line, the attenuation per pass for an RT60 T is
  g(T) = 10^(−3·L/(fs·T)) (L the line length in samples). Three regions:
  T_low = decay·low_decay below `crossover`, T_mid = decay, T_high =
  decay·high_decay above `damping_freq`; realize g(T_mid) × a second-order
  Butterworth-Q low shelf at `crossover` (DC gain g(T_low)/g(T_mid)) × a
  second-order Butterworth-Q high shelf at `damping_freq` (Nyquist gain
  g(T_high)/g(T_mid)). Coefficients
  recomputed only while decay/size/shelf parameters move (fixed 16-sample
  grid, as the EQ).
- **Modulation:** each line's read position moves by seeded slow sinusoids
  (0.1..1.2 Hz, distinct rates and phases), depth up to ±0.5 ms at
  `modulation` 100 %; 0 is exactly none.
- **Width:** mid/side on the whole wet (early + late), S × width/100.
- **`freeze`:** input to the FDN and early taps fades out over 10 ms; decay
  gains glide to 1 and the shelves flat (lossless loop), modulation fades
  out, size holds. Off restores the decay over 10 ms.
- **`mix`/`output`:** linear dry/wet crossfade, output gain; smoothed.
- **Tail:** the CLAP tail extension as the delay's: infinite while frozen,
  otherwise predelay + 1.5 × the longest band RT60 + 100 ms.
  `clap_host_tail::changed` runs from `process()` on the audio thread;
  main-thread parameter/state edits set an atomic pending flag.
- **CPU:** ≤ 2 % of a core per stereo instance at 48 kHz, 512-frame blocks,
  -O2 (a bench executable as the EQ's, not a test); report the number.
- Opt-in `reverb-engine.measure`: full RT60 per octave band (125 Hz … 8 kHz;
  octave-band filtered impulse response, Schroeder backward integration, T30 fit)
  within 10 % of the three-region target for bands at least an octave from
  `crossover`/`damping_freq`, at decay 0.5/2/8 s and size 20/50/90;
  normalized echo density (Abel & Huang) of the impulse response reaches
  ≥ 0.8 within 150 ms at the defaults, and earlier with more diffusion
  (report the measured times); a mono input gives a decorrelated tail
  (interchannel correlation of the late tail < 0.3 at width 100, > 0.99 at
  width 0); freeze holds the tail RMS within 1 dB over 10 s; `early` at its
  minimum removes the early taps exactly; tail values; defaults bit-exact
  across block sizes.
- Routine CTest (beyond §1.7) uses representative 48 kHz RT60 cases: decay
  0.5/2 s at size 50 and 0.5 s at size 90, bands 250 Hz / 1 kHz / 4 kHz
  outside active transitions. Shortened IRs still reach the T30 fit floor;
  density/stereo and modulated-decay checks use shorter representative tails.
  Keep the ten-second freeze comparison and five-second automation stress;
  aggregate per-sample predicates. No raised timeout: targets ≤10 s dev,
  ≤120 s TSan, with the dense matrix retained in the opt-in runner.
- GUI: the decay curve (RT60 against frequency, log axes, from the three
  regions and the cut filters), size and decay large, predelay, diffusion,
  early, modulation, low_decay/crossover, high_decay/damping_freq,
  low_cut/high_cut, width, freeze, mix, output. Scenarios: `default`,
  `hall` (size 85, decay 4500 ms, predelay 25 ms, high_decay 0.4).

## 6. Engine and benchmark use

Once built, the plugins are in the build's `plugins/clap/`; the engine
finds them when its CLAP search path includes that directory. B1 proper
uses the EQ and the compressor on every track and the compressor as a peak
catcher on the master (docs/benchmarks/); the MCP arrangement acceptance test gains a mixing pass
with them in a later round.
