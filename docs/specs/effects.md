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
| `contour` (`plugins/stock/contour`) | Contour | `com.suppleaudio.contour` | `TQNk` (the player's for the id) | `Cntr` | audio-effect, equalizer |
| `comp` | Ballast | `com.suppleaudio.comp` | `QCM1` | `QCmp` | audio-effect, compressor, limiter, stereo |
| `utility` | Plumb | `com.suppleaudio.utility` | `QUT1` | `QUtl` | audio-effect, utility, stereo |
| `analyzer` | Gauge | `com.suppleaudio.analyzer` | `QAN1` | `QAna` | audio-effect, analyzer, stereo |
| `drive` | Kiln | `com.suppleaudio.drive` | `QDR1` | `QDrv` | audio-effect, distortion, stereo |
| `gate` | Sluice | `com.suppleaudio.gate` | `QGT1` | `QGat` | audio-effect, gate, expander, stereo |
| `transient` | Flint | `com.suppleaudio.transient` | `QTS1` | `QTrn` | audio-effect, transient-shaper, stereo |
| `delay` | Echelon | `com.suppleaudio.delay` | `QDL1` | `QDly` | audio-effect, delay, stereo |
| `reverb` | Nave | `com.suppleaudio.reverb` | `QRV1` | `QRvb` | audio-effect, reverb, stereo |

In the template's identity checklist the code name is the id
(`Comp`/`comp`/`COMP`, `Utility`/`utility`/`UTILITY`,
`Analyzer`/`analyzer`/`ANALYZER`); only the user-visible name string is the
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
  Utility sanitizes its working inputs before DC/side-filter state updates,
  while its exact unity output copy preserves the original sample bits.
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
a gesture-bracketed parameter change. Utility's discrete arrow-key edits step
one physical value, independent of fine mode; floats retain normalized steps.
`tools/gui_preview` renders at least two scenarios per plugin to PNG for review.
Design size ≤ 900 × 560. The look follows the product theme used by Orrery
(`plugins/orrery/plugin/gui/Theme.h`): same palette and type, so the stock
plugins read as one family.

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

## 2. EQ — Contour [`contour`] (`stock-effects.md` §1)

A DSP script (ADR-0047), `plugins/stock/contour/script/contour.cpp`; the
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
- **Tests.** `plugin.contour.response` measures bells, cuts and shelves
  through the built plugin; `plugin.contour.catalog` keeps `Catalog.cpp` in
  step; the template's suite, reset, unload, semantic conformance and
  clap-validator checks run as for every stock plugin. During development,
  `dsp.test` with `automation` (no excess step on type, channel or enable
  switches) and `dsp.preview` with `pointer` steps covered clicks and every
  gesture.

## 3. Compressor / limiter — Ballast [`comp`] (`stock-effects.md` §2)

Ports: main stereo in/out plus a second, non-main stereo input "Sidechain"
(`CLAP_PORT_STEREO`, not `CLAP_AUDIO_PORT_IS_MAIN`), as ADR-0018 hosts
expect. With `sidechain: external` and no key (or silence), nothing is
reduced.

Compressor mode:
- Detector: the key (internal = main input) through `sc_highpass` and
  `sc_lowpass` (12 dB/oct, `off` at their end values), per channel level
  (peak |s|, or RMS over `rms_window`), stereo link
  `link·max(L, R) + (1 − link)·own`. `feedback` topology detects on the
  output of the previous sample instead.
- Gain computer in dB with the quadratic soft knee (Giannoulis, Massberg &
  Reiss 2012): below T − W/2: 0; above T + W/2: (1/R − 1)(x − T); inside:
  (1/R − 1)(x − T + W/2)² / (2W). Ratio 100 is ∞.
- Ballistics on the reduction in dB: branching one-pole, α = exp(−1/(t·fs)),
  attack when the target is deeper, release otherwise. `auto_release`: a
  two-stage release (fast `release`, slow 5× `release`) weighted by how long
  the reduction has been held — document the rule in `stock-effects.md`.
- `auto_makeup`: compensates the static reduction at −18 dBFS input (so a
  typical level is roughly level-matched); documented.
- Lookahead delays the audio, the dry path of `mix`, and filtered-key audition
  by the reported latency, but not the detector. A prepared key delay stays
  populated while audition is off, so audition and key-source transitions
  retain the same sample alignment; latency = lookahead samples.

Limiter mode (ratio ∞, the compressor's threshold ignored, `ceiling` sets
the output):
- Detector per channel on the input; with `true_peak`, on the 4× interpolated
  signal from `dsp::PolyphaseUpsampler` (the interpolator `core-analysis`
  uses for BS.1770 true peak; its delay of 16 input samples is added to the
  audio path and to the reported latency).
- Target gain `min(1, ceiling/peak)`; the minimum over the lookahead window
  (sliding-window minimum, preallocated monotonic deque), then a moving
  average over the same window (the gain is fully down when the peak reaches
  the output), then release with `release`, never above the windowed target.
  With `lookahead: off` the limiter is a zero-latency peak limiter with
  instant attack (only sample peaks are guaranteed, by the final clip).
- A final clip at the ceiling catches rounding; no output sample exceeds it.

Tests: static curve (steady sines −60…0 dBFS within 0.2 dB of the gain
computer, knee included); attack/release (63 % of a 20 dB change in the set
time ±10 %); external key (silent main keyed loud → no change; loud main
keyed by silence → no reduction); `sc_highpass` 200 Hz (a 50 Hz key reduces
< 1 dB where a 1 kHz key at the same level reduces fully); lookahead
alignment and reported latency; limiter ceiling: sine bursts, clipped
squares, +20 dBFS noise and a 0.25·fs sine with inter-sample peaks come out
≤ ceiling + 0.1 dBTP (measured with `core-analysis`, lookahead ≥ 1 ms,
`true_peak` on) and never above the ceiling in sample peak; a −10 dBFS sine
under a −1 dB ceiling passes delayed and otherwise within 1e-6; recovery
after a 10 ms +12 dB burst within 0.1 dB after ≈ 5·release.

GUI: transfer curve (input vs output dB, knee, a moving dot for the current
level), gain-reduction history (~4 s), controls grouped as detector,
sidechain, output, with the mode switch prominent; the limiter mode shows
the ceiling and input/output peak meters. Scenarios: `default`, `heavy`
(ratio 10, knee 0, external key), `limiter`.

## 4. Utility — Plumb [`utility`] (`stock-effects.md` §8)

- Order: channels/swap → polarity → dc block → width and mono-below (M/S)
  → pan → gain → mute (10 ms ramps for mute and channel changes).
- `pan` is a balance: the opposite side falls, 0 dB at centre (document
  the law).
- **Change to the spec:** `mono_below` removes the side below the crossover
  with a 4th-order Linkwitz–Riley high-pass on S only — zero latency and M
  untouched, so the mono sum is exact and nothing needs a linear-phase pair
  or a stepped parameter (it stays automatable). Write this into
  `stock-effects.md` §8.
- Tests: defaults bit-exact; −inf gain is silence, +6 dB doubles; width 0 =
  (L+R)/2 on both, width 200 doubles S; pan +100: left silent, right
  unchanged; mono_below 150 Hz: side of a 50 Hz tone below −40 dB, of a
  1 kHz tone within 0.1 dB, and the output's mono sum equals the input's.
- GUI: one compact panel (≤ 520 × 300): gain and pan, width, channel mode,
  swap, polarity and mute toggles, mono-below, dc block, a small
  correlation/level meter from telemetry. Scenarios: `default`, `wide`.

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

## 7. Second wave: saturation — Kiln [`drive`] (`stock-effects.md` §5)

Identity: display name Kiln, id `drive`, CLAP id `com.suppleaudio.drive`,
state magic `QDR1`, AU subtype `QDrv`, features audio-effect, distortion,
stereo. Everything in §1 applies.

- **Oversampling** (`1x`, `2x`, `4x`, `8x`; stepped, latency): a shared
  primitive this plugin adds to `libs/core-dsp` (the only worker touching
  it): `dsp::Oversampler` — cascaded linear-phase half-band FIR stages
  (design documented in the header: stopband ≥ 100 dB, passband to 0.45 of
  the base rate within ±0.01 dB; the stage next to the base rate stops from
  0.55·fs, later stages only from their input rate − 0.45·fs, so they stay
  short — corrected 2026-09-30 after a 1025-tap first stage cost 12 ms of
  latency), up and down per channel, preallocated for
  the maximum block, `latency()` exact in base-rate samples (an integer:
  pad the stage delays so it is). Tests in `libs/core-dsp`: passband,
  stopband (images and aliases), exact latency, block-size invariance.
- **Curves** with first-order antiderivative anti-aliasing (ADAA): for
  f(x) with antiderivative F, y[n] = (F(x[n]) − F(x[n−1])) / (x[n] − x[n−1]),
  falling back to f((x[n] + x[n−1]) / 2) when |x[n] − x[n−1]| < 1e-5; this
  adds half a sample of delay at the oversampled rate, which the dry path and
  the reported latency include (round the total to whole base-rate samples
  by adding delay). Curves (all odd-symmetric before `bias`, unity small-
  signal slope, bounded): `tape` (tanh-like soft curve with a slightly
  wider knee), `tube` (a gentler onset than tape and stronger compression when
  driven; like every curve it is odd-symmetric, and its even harmonics come
  from `bias`), `transistor` (harder knee, x/(1+|x|ⁿ)
  family), `diode` (exponential knee), `fold` (smooth sine-based wavefolder;
  its antiderivative is closed-form), `hard` (clip at ±1). Write each curve
  and its antiderivative in `stock-effects.md` §5.
- **Drive/bias/tone:** input × drive (dB); `bias` adds a DC offset before
  the curve (asymmetry → even harmonics) and a DC blocker (5 Hz) after it;
  `tone` is a first-order tilt before the curve and its inverse after
  (so the dry tone is kept at low drive).
- **`auto_gain`:** divide by the curve's small-signal gain model at the
  current drive (analytic per curve, documented), verified to hold loudness
  within 1 LU on pink noise across the drive range (core-analysis LUFS).
- **`mix`/`output`:** dry path delayed by the full latency; mix and output
  smoothed.
- Tests (beyond §1.7): THD rises monotonically with drive for each curve;
  aliasing of a 5 kHz sine at 24 dB drive, `4x`, 48 kHz: every non-harmonic
  component in 20 Hz–20 kHz below −90 dB relative to the fundamental
  (aliases between 20 kHz and Nyquist are accepted); `bias` 0 gives no
  even harmonics above −100 dB for odd curves, `bias` 50 % gives a 2nd
  harmonic above −40 dB; DC-free output (mean below 1e-5 after 1 s); latency
  reported equals the measured delay for every factor at 44.1/48/96 kHz;
  `mix` 50 % on a sine at drive 0 dB and `tape` equals the input within
  0.01 dB (alignment check).
- GUI: the transfer curve (input vs output, with a moving dot) and a
  harmonic spectrum bar display of the current setting (computed from a
  test sine on the main thread), drive large, curve selector, bias, tone,
  oversampling, auto gain, mix, output. Scenarios: `default`, `hot` (fold,
  24 dB, bias 30 %).
- Orrery keeps its own minimum-phase decimator (voices are generated at the
  oversampled rate); do not change Orrery.

## 8. Second wave: gate / expander — Sluice [`gate`] (`stock-effects.md` §3)

Identity: display name Sluice, id `gate`, CLAP id `com.suppleaudio.gate`,
state magic `QGT1`, AU subtype `QGat`, features audio-effect, gate,
expander, stereo. Everything in §1 applies. Ports as the compressor (§3):
main stereo plus a non-main "Sidechain" input.

- Detector as the compressor's (key filters `sc_highpass`/`sc_lowpass`,
  peak, stereo link; `sc_listen` outputs the filtered key). Reuse nothing by
  linking across plugins: copy the few lines you need into this engine.
- **Gate:** a state machine per linked detector — opens when the level
  rises above `threshold`, closes when it falls below `threshold −
  hysteresis` and `hold` has elapsed since it last exceeded the close level.
  The gain moves between 0 dB and `range` with `attack` (opening) and
  `release` (closing) as one-pole ramps in the dB domain (the floor is
  `range`, not silence, unless range is −80 → treat −80 as −inf: exact zero).
- **Expander:** downward expansion below `threshold` by `ratio`
  (gain = (ratio − 1)·(level − threshold) dB, floored at `range`), with the
  same attack/release ballistics and hysteresis on the threshold.
- `lookahead` (`off`/`1ms`/`5ms`, stepped): delays the audio so the gate is
  open when a transient arrives; latency = lookahead samples.
- Readonly `gr` (dB) and `open` (bool) update at most every 50 ms.
- Tests: a burst above threshold opens within attack (±10 %) and closes
  after hold + release; hysteresis prevents chatter on a signal oscillating
  ±2 dB around the threshold (count transitions); range −40 dB floors the
  closed gain at −40 ± 0.1 dB, range −80 gives exact zeros; external key
  gating a silent-keyed input keeps it closed; lookahead: the transient's
  first sample passes at full gain; expander static curve within 0.2 dB.
- GUI: a level/threshold meter with the hysteresis band, open/closed
  indicator and gain history, controls grouped as the compressor's.
  Scenarios: `default`, `drums` (hold 50 ms, range −30, key high-pass).

## 9. Second wave: transient shaper — Flint [`transient`] (`stock-effects.md` §4)

Identity: display name Flint, id `transient`, CLAP id
`com.suppleaudio.transient`, state magic `QTS1`, AU subtype `QTrn`,
features audio-effect, transient-shaper (plus `utility` if CLAP has no
closer feature), stereo. Everything in §1 applies; no sidechain port.

- Detector: the stereo-linked rectified signal (max |L|, |R|) of the input.
  Two envelope followers in the dB domain: fast (attack 0.1 ms scaled by
  `speed`/10, release `speed`) and slow (attack `speed`, release 4·`speed`).
  Their difference d = fast − slow (dB) is positive at onsets, negative in
  decays.
- Gain (dB): onset part = (attack/100)·12·clamp(d/10·(sensitivity/50), 0, 1),
  sustain part = (sustain/100)·12·clamp(−d/10·(sensitivity/50), 0, 1),
  total = onset + sustain, smoothed with a 1 ms one-pole. This keeps the
  shaping level-independent (the detector works on dB differences) and
  bounds it to ±12 dB per part as the spec says; document the final formula
  in `stock-effects.md` §4 if you refine it.
- `clip`: a soft clip (tanh-style, unity small-signal slope) after the gain,
  with 2× oversampling of the clip stage only (latency reported; 2× via the
  shared `dsp::Oversampler`, 72 samples round-trip delay).
- `mix` (dry aligned with any latency) and `output`.
- Tests: attack +100 on a synthetic drum hit (a 5 ms 1 kHz burst on a decay)
  raises the first 5 ms by ≥ 6 dB and leaves the tail within 1 dB; sustain
  −100 lowers the tail by ≥ 6 dB and leaves the onset within 1 dB;
  level independence: the same hit 10 dB louder gets the same gain curve
  within 0.5 dB; a steady sine gets no gain change after settling (< 0.1 dB);
  defaults are bit-exact.
- GUI: the input and output envelopes of the last ~2 s with the gain curve,
  attack and sustain large (bipolar arcs from the centre), speed,
  sensitivity, clip, mix, output. Scenarios: `default`, `punch`
  (attack +60, sustain −30).

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

- **`early`** dB uses the utility's convention: a finite −60 dB endpoint
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
uses the EQ and the compressor on every track and the limiter on the master
(docs/benchmarks/); the MCP arrangement acceptance test gains a mixing pass
with them in a later round.
