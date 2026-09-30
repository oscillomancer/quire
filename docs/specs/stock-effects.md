# Spec: stock effects (Phase 3, after the synth)

**Status:** Draft for review (2026-09-26). Each plugin's section is frozen
when its implementation starts.
Implementation of §1, §2, §8 and §9 started 2026-09-29 (plan and
implementation notes: `docs/specs/effects.md`); those sections are frozen
except for the decisions recorded there.
**Common rules:** ADR-0010 (semantic params, value domains), ADR-0011
(template instantiation), `docs/specs/core-params.md`, `core-patch.md`,
`semantic-params-extension.md`.

## 0. Conventions shared by every stock effect

- **Patch form:** flat (core-patch `FlatPatchType`): `"params": {"<key>":
  value}` with dotted keys (`band2.freq`). One form for every effect, so an
  agent learns it once. Keys and units are the parameter table.
- **Names:** working names below are proposals (craft nouns, like Nate's
  products); code identifiers use the functional id in brackets until Nate
  confirms (`HUMAN_REVIEW.md` #9). CLAP ids `com.suppleaudio.<id>`.
- **Every effect:** mono/stereo configs (plus sidechain input where
  listed), float/double, sample-accurate automation, block-size invariant
  (null −120 dBFS across 1..4096), denormal-safe, deterministic (seeded where
  random), latency reported exactly (with compensated bypass), `mix`
  (dry/wet, latency-aligned) and `output` (dB) where meaningful, a
  `describe()` sentence, RT-safe under rtsan, CLAP validator clean, loads
  and automates in REAPER (acceptance, Phase 3 §acceptance).
- **Latency-changing parameters** (lookahead, oversampling, linear phase)
  are stepped, not automatable, and follow the template's restart contract
  (`request_restart`, reactivate with the new latency).
- **Readonly telemetry parameters** (gain reduction, levels) exist for
  generic hosts; the GUI uses the telemetry ring.
- **Tests beyond the template's:** frequency/phase response against analytic
  targets, THD/aliasing measurements where nonlinear, envelope timing
  against spec, sidechain behaviour, parameter-sweep automation stress, and
  golden renders (tolerance-aware null) for every factory preset.

## 1. EQ — "Facet" [`eq`]

Parametric EQ with 8 bands, per-band stereo placement, analysis-driven
workflow in mind ("cut 250 Hz by 3 dB, Q 1.2" is one merge patch).

| key | type | range | curve | default |
|---|---|---|---|---|
| `bandN.enabled` (N = 1..8) | bool | | | band1–4 on, 5–8 off |
| `bandN.type` | enum | `bell`, `low_shelf`, `high_shelf`, `high_pass`, `low_pass`, `notch`, `band_pass`, `tilt` | | 1 `high_pass`, 2 `bell`, 3 `bell`, 4 `high_shelf`, 5–8 `bell` |
| `bandN.freq` | Hz | 10..30000 (clamped < 0.49 fs) | log | 30, 200, 1000, 8000, 500, 2000, 4000, 12000 |
| `bandN.gain` | dB | −30..+30 | linear | 0 |
| `bandN.q` | none | 0.1..24 | log | 0.71 |
| `bandN.slope` | enum | `6`, `12`, `18`, `24`, `36`, `48`, `72`, `96` (dB/oct; HP/LP only) | | `24` |
| `bandN.channel` | enum | `stereo`, `left`, `right`, `mid`, `side` | | `stereo` |
| `phase` | enum | `minimum`, `natural` (analog-matched magnitude near Nyquist), `linear` (latency; stepped) | | `natural` |
| `auto_gain` | bool | output level compensation for the static curve | | false |
| `output` | dB | −24..+24 | linear | 0 |

DSP: `natural` uses matched-magnitude biquads (analog prototype magnitude
matched at DC, centre and Nyquist, or 2× oversampling — whichever the
implementation proves more exact in tests), `minimum` plain bilinear with
prewarping, `linear` a partitioned FIR from the minimum-phase design (fixed
latency 2048 samples at 48 kHz, scaled with rate). Bell/shelf gains follow
the analog prototype within 0.1 dB up to 20 kHz at 48 kHz in `natural`.
Cascaded HP/LP slopes are Butterworth (odd orders include a first-order
section). Coefficient changes smoothed at control rate (no zipper noise
under full-range sweeps, verified by a spectral test).
GUI centrepiece: the summed response over a live spectrum (core-analysis
LTAS of the input, pre/post).

## 2. Compressor / limiter — "Ballast" [`comp`]

Inputs: main stereo + sidechain stereo (CLAP port 1).

| key | type | range | curve | default |
|---|---|---|---|---|
| `mode` | enum | `compressor`, `limiter` | | `compressor` |
| `threshold` | dB | −60..0 | linear | −18 |
| `ratio` | ratio | 1..100 (100 shown as ∞) | pow 3 | 4 |
| `knee` | dB | 0..24 | linear | 6 |
| `attack` | ms | 0.01..300 | log | 10 |
| `release` | ms | 5..5000 | log | 150 |
| `auto_release` | bool | program-dependent release | | false |
| `detector` | enum | `peak`, `rms` | | `peak` |
| `rms_window` | ms | 1..100 | log | 10 |
| `topology` | enum | `feedforward`, `feedback` | | `feedforward` |
| `sidechain` | enum | `internal`, `external` | | `internal` |
| `sc_highpass` | Hz | 10..500 (10 = off) | log | 10 |
| `sc_lowpass` | Hz | 1000..20000 (20000 = off) | log | 20000 |
| `sc_listen` | bool | | | false |
| `stereo_link` | % | 0..100 | linear | 100 |
| `lookahead` | enum | `off`, `1ms`, `5ms` (latency) | | `off` |
| `makeup` | dB | −12..+24 | linear | 0 |
| `auto_makeup` | bool | | | false |
| `ceiling` | dB | −24..0 (limiter mode: output ceiling) | linear | −1 |
| `true_peak` | bool | limiter mode: 4× oversampled detection | | true |
| `mix` | % | 0..100 | linear | 100 |
| `output` | dB | −24..+24 | linear | 0 |
| `gr` (readonly) | dB | −60..0 | | |

DSP: log-domain gain computer with quadratic soft knee; attack/release
one-pole smoothing in the dB domain (branching); limiter mode = ratio ∞
with lookahead peak hold + release, true-peak mode guarantees output ≤
ceiling dBTP on the Tech 3341 TP signals and on programme material (tested
with core-analysis true peak).

## 3. Gate / expander — "Sluice" [`gate`]

Sidechain input as for the compressor. Keys: `mode` (`gate`, `expander`),
`threshold` dB −80..0 (−40), `range` dB −80..0 (−80; attenuation floor),
`ratio` 1..10 (expander, 2), `hysteresis` dB 0..12 (3), `attack` ms
0.01..100 log (0.5), `hold` ms 0..1000 (20), `release` ms 5..5000 log (150),
`lookahead` enum `off`/`1ms`/`5ms`, `sidechain`, `sc_highpass`,
`sc_lowpass`, `sc_listen`, `stereo_link` %, `mix` %, `output` dB, readonly
`gr` dB, readonly `open` bool.

### Implementation decisions (2026-09-29)

- `mode` defaults to `gate`. Shared detector/output keys use the compressor's
  ranges/defaults: `sidechain` internal/external (internal), `sc_highpass`
  10..500 Hz log (10 = off), `sc_lowpass` 1000..20000 Hz log (20000 = off),
  `sc_listen` false, `stereo_link` 0..100 % (100), `mix` 0..100 % (100),
  `output` −24..24 dB (0). `ratio` and `hold` have linear host curves.
  Lookahead machine keys are `off`/`ms1`/`ms5`, labels off/1 ms/5 ms,
  following the lowercase-letter enum-key convention; default off.
- The key uses 12 dB/oct Butterworth TPT high/low-pass sections. Corners
  clamp to 0.45 fs internally. Linking blends each channel's absolute key
  with the stereo maximum in amplitude, before converting to dB. A missing
  external key is silence; the sidechain bus remains stereo in mono mode.
- Gate state opens strictly above threshold, refreshes hold at or above
  threshold minus hysteresis, and closes after that sample-counted hold.
  `open` reports either channel's logical state, independent of gain ramp.
  The expander uses the same state/hold: while latched open it has unity
  target gain; while closed it uses `(ratio−1)·(level−threshold)`, capped
  between range and 0 dB. Thus its hysteresis band is history-dependent.
- Attack/release are one-pole time constants in the dB domain: 63.2 % of
  the change after one time constant, not full settling. A −80 dB range is
  exact mute initially closed and once within 0.0001 dB of the closing
  floor; finite ranges retain their stated gain. Dry mix remains audible.
- Lookahead is rounded to samples, delays both main and monitored key,
  and aligns the dry path. Opening normalizes the one-pole ramp to the
  available lookahead so it reaches unity before the delayed onset, even
  if attack is longer. Hold is at least lookahead + one sample to preserve
  short onsets. With lookahead off, the attack time constant is unmodified.
  Active edits request a coalesced host restart; both processing delay and
  reported latency remain the activated value until reactivation.
- Continuous controls ramp over 20 ms (key frequency in log space); mode,
  key source, listen and key-filter enable transitions ramp over 10 ms.
  Filter/time coefficients update on a reset-counted 16-sample grid.
  Restoration before processing snaps. Nonfinite input becomes zero and
  input magnitudes above 1e8 are bounded before they enter recursive state.
  Activation also supports the validator's 8 kHz case; lookahead alignment
  is tested up to 768 kHz in addition to the required rate sweep.
- Key listen replaces the wet signal with the filtered, delayed key; mix
  and output still apply. Readonly `gr` is −80..0 dB, the deepest channel
  gain before mix/output; `open` is bool. Both publish every 50 ms. Editor
  key level is the current linked detector sample at that same cadence.

## 4. Transient shaper — "Flint" [`transient`]

Keys: `attack` % −100..+100 (0; boost/cut of the transient portion, ±12 dB
at the extremes), `sustain` % −100..+100 (0), `speed` ms 1..50 log (10;
detector time constant separating attack from sustain), `sensitivity` %
0..100 (50), `clip` bool (soft clip output), `mix` %, `output` dB. DSP: dual
envelope followers (fast/slow) on a detector signal; gain = f(fast − slow);
level-independent (a 10 dB louder input yields the same shaping).

## 5. Saturation — "Kiln" [`drive`]

Keys: `curve` enum `tape`, `tube`, `transistor`, `diode`, `fold`, `hard`
(`tape`); `drive` dB 0..36 (6); `bias` % −100..100 (0; asymmetry → even
harmonics); `tone` dB/oct −6..+6 (0; tilt before/after pair keeps the
dry-matched tone); `oversampling` enum `1x`, `2x`, `4x`, `8x` (`4x`;
latency); `auto_gain` bool (true; equal loudness by an analytic
small-signal gain model, verified with core-analysis LUFS within 1 LU over
the drive range on pink noise); `mix` %, `output` dB. DSP: first-order
antiderivative anti-aliasing (ADAA) for every static curve plus
oversampling; `fold` uses a smooth wavefolder with ADAA. Tests: THD vs drive
curves per `curve`, aliasing below −90 dB for a 5 kHz sine at `4x`/24 dB
drive, DC-free output (bias adds DC blocking after the nonlinearity).

## 6. Delay — "Echelon" [`delay`]

Keys: `sync` enum `off` + note values (as in the synth spec) (`1/8.`),
`time` ms 1..5000 log (375; used when `sync` off), `mode` enum `stereo`,
`ping_pong`, `mono` (`stereo`), `offset` % −50..+50 (0; right-channel time
offset relative to left), `feedback` % 0..110 (35; > 100 is safety-limited
self-oscillation), `highpass` Hz 20..2000 log (80), `lowpass` Hz
1000..20000 log (8000) (in the feedback loop), `mod_rate` Hz 0.05..10 log
(0.5), `mod_depth` % 0..100 (0; tape-style wow), `drive` dB 0..24 (0; loop
saturation), `duck` dB 0..24 (0; wet ducking under the input), `width` %
0..200 (100), `freeze` bool, `mix` % (25), `output` dB. DSP: fractional
delay (cubic/allpass), smooth time changes (tape-like glide, no clicks),
tempo sync via CLAP transport.

## 7. Reverb — "Nave" [`reverb`]

Keys: `size` % 0..100 (50), `decay` ms 100..60000 log (2000; RT60 at mid
frequencies), `predelay` ms 0..500 (10), `low_decay` x 0.25..4 log (1;
RT60 multiplier below `crossover`), `high_decay` x 0.1..1 log (0.5; above
`damping_freq`), `crossover` Hz 100..1000 log (250), `damping_freq` Hz
1000..16000 log (6000), `diffusion` % 0..100 (70), `early` dB −inf..+6 (−6;
early reflections level), `modulation` % 0..100 (20), `width` % 0..200 (100),
`low_cut` Hz 20..1000 log (20), `high_cut` Hz 1000..20000 log (16000),
`freeze` bool, `mix` % (20), `output` dB. DSP: an original 16-line feedback
delay network with a Householder (or Hadamard) mixing matrix, per-line
two-band decay filters derived from the RT60 targets, input allpass
diffusion, seeded line-length jitter and modulation; tests measure RT60 per
octave band against the parameters (within 10 %) and echo density.

## 8. Utility — "Plumb" [`utility`]

Keys: `gain` dB −inf..+24 (0), `pan` % −100..100 (0), `width` % 0..200 (100),
`invert_left`, `invert_right` bool, `swap` bool, `channels` enum `stereo`,
`left_to_both`, `right_to_both`, `mid`, `side` (`stereo`), `mono_below` Hz
0..500 (0 = off; bass mono: a 4th-order Linkwitz–Riley high-pass on the
side only — zero latency, the mid untouched so the mono sum is exact;
automatable; amended 2026-09-29, `docs/specs/effects.md` §4), `dc_block`
bool, `mute` bool.

Implementation decisions (2026-09-29):
- `gain` uses a finite −120 dB endpoint with `kOffAtMin`: that endpoint
  means exact silence and displays −inf; all other values are finite dB.
  +6 dB means 10^(6/20), approximately 1.9953; exact doubling is
  20·log10(2), approximately 6.0206 dB.
- `mono_below` has a linear Hz host curve because its range includes zero.
- Pan is linear balance: L × (1 − max(0, pan/100)),
  R × (1 + min(0, pan/100)); centre leaves both at unity.
- Channel selection sees the swapped input. `mid` and `side` send
  (L+R)/2 or (L−R)/2 respectively to both outputs.
- DC blocking is a 5 Hz one-pole high-pass. Gain, balance, width and the
  crossover use finite 20 ms ramps; routing, polarity, DC enable,
  crossover enable and mute use 10 ms ramps. Restored values snap before
  the first processed sample. Gain ramps in amplitude to reach exact zero.
- An LR4 high-pass at 150 Hz attenuates 50 Hz by approximately 38.3 dB,
  rather than the implementation plan's 40 dB. Tests use the exact
  prewarped digital LR4 response (±0.02 dB), and require < 0.1 dB loss
  at 1 kHz. The filter operates only on S and has zero reported latency.
- Append-only readonly parameters `input_level` and `output_level`
  (dBFS, −120..40), and `correlation` (−1..1, silence = 0), publish
  once per 50 ms. Correlation is the normalized output cross-product.

## 9. Analyzer — "Gauge" [`analyzer`]

Passes audio unchanged (bit-exact; tested). Publishes live analysis:
streaming `LoudnessMeter` (M/S/I, LRA, TP), correlation/width/balance,
a smoothed 1/3-octave spectrum and spectral centroid — through (a) readonly
CLAP params for the scalar values (generic hosts can record them), (b) the
GUI telemetry ring, and (c) a semantic-params extension addition
`get_telemetry(plugin, out)` (appended member, §4 versioning rule) that
writes a compact `supple.analysis-report/1.0`-shaped JSON of the last N
seconds (`window` param: 3 s, 10 s, 30 s, since reset) so the DAW and agents
can read live measurements of any point in the graph. Keys: `window` enum,
`reset` (momentary bool), `hold` bool.

### Implementation decisions (2026-09-29)

- `window` choices are `s3` (default), `s10`, `s30`, `since_reset`; changing
  it clears history. Hold freezes measurement time and excludes new audio;
  reset is a momentary trigger, returns to false, and clears even while held.
- Scalar keys: `integrated`, `short_term`, `momentary` (LUFS −120..40),
  `lra` (LU 0..120), `true_peak` (dBTP −120..40), `correlation` (−1..1),
  `width` (percent 0..100), `balance` (dB −120..120, right/left power ratio),
  `centroid` (Hz 0..20000). All readonly; unavailable loudness/peak = −120,
  LRA = 0, silence correlation = 1. Width is side/(mid+side) energy × 100.
- Measurements run on a joined consumer worker, never the audio callback.
  The preallocated SPSC queue holds at least two seconds; it drops new
  packets under overload and restarts history after a gap. Output stays
  bit-exact even for nonfinite samples; only measurement copies are sanitized.
- Rolling loudness/stereo/peaks use a sample-counted 100 ms grid. Integrated
  and LRA retain complete 400 ms/3 s blocks wholly inside the selected horizon;
  since-reset uses bounded histograms (LRA resolution 0.01 LU). True peak
  reuses core-analysis's 4× at ≤48 kHz, 2× at ≤96 kHz, 1× above policy.
- Spectrum is the mid signal's 31 ISO third-octave bands (20 Hz–20 kHz),
  Hann FFT (~43 ms) on the 100 ms grid, smoothed in power with a 300 ms
  time constant. The current spectral centroid uses that same power spectrum.
  This is a live spectrum rather than a full-window offline LTAS.
- Telemetry JSON contains only measured descriptors, follows the existing
  analysis-report field names and adds `live: {window, hold, dropped_frames}`.
  Current M/S are single-element series; spectrum is `spectral.ltas_db`.
  Silence/unavailable values use finite meter floors. Generic-host scalar
  events are no more frequent than 50 ms; GUI frames exist only when visible.

## 10. Order of work and acceptance

After Orrery reaches its evaluation milestone: EQ → compressor/limiter →
utility + analyzer (needed for mixing tasks) → saturation → gate →
transient → delay → reverb. Each plugin is done when its tests, the CLAP
validator, the RT sanitizer run, a REAPER load/automation check (Nate or an
agent via computer-use), and its skill page exist.
