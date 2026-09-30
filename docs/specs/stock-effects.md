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

- **Enum keys** start with a lowercase letter (core-params validation):
  numeric choices take a unit prefix — `db12`, `ms5`, `x4` — while their
  display labels keep the numbers ("12 dB/oct", "5 ms", "4x"). Tables below
  that still show digit-led keys mean these prefixed forms.
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
| `bandN.slope` | enum | `db6`, `db12`, `db18`, `db24`, `db36`, `db48`, `db72`, `db96` (dB/oct; HP/LP only) | | `db24` |
| `bandN.channel` | enum | `stereo`, `left`, `right`, `mid`, `side` | | `stereo` |
| `phase` | enum | `minimum`, `natural` (analog-matched magnitude near Nyquist), `linear` (latency; stepped) | | `natural` |
| `auto_gain` | bool | output level compensation for the static curve | | false |
| `output` | dB | −24..+24 | linear | 0 |

DSP: `natural` uses filter-specific matched second-order sections at the host
sample rate; `minimum` uses bilinear SVFs with prewarping; `linear` uses a
partitioned FIR from the minimum-phase design (fixed latency 2048 samples at
48 kHz, scaled with rate). The natural magnitude target is ≤0.3 dB deviation
from the analog prototype through 20 kHz at 44.1/48 kHz for bells with Q ≤4
and shelves with Q ≤1, f0 ≤10 kHz, over the full ±30 dB gain range. Resonant
shelves and near-Nyquist bands have the measured limits below, rather than the
same accuracy guarantee. Cascaded HP/LP slopes use Butterworth sections (odd
orders include a first-order section). Float changes have 20 ms smoothing;
matched coefficients update on a fixed 16-sample grid only while moving.
GUI centrepiece: the summed response over a live spectrum (core-analysis
LTAS of the input, pre/post).

### EQ implementation decisions (2026-09-30)

- Approved correction: slope enum keys are `db6`…`db96`, with dB/oct display
  labels. Numeric-only enum keys violate core-params validation.

- The authoritative 0.49·fs frequency clamp here takes precedence over the
  generic implementation plan's 0.45·fs. Values in patches remain unclamped.
- Natural mode uses original matched sections derived from the mathematical
  method in [Vicanek (2016)](https://vicanek.de/articles/BiquadFits.pdf), with
  no oversampling or half-band filters. Main boost poles use impulse invariance.
  Bell numerators match DC, centre magnitude and the stationary centre. Shelf
  numerators match DC, Nyquist and the midpoint; Q >1 uses the analog bump.
  An unrealizable resonant shelf numerator, or one that misses the opposite
  extremum by >0.5 dB, falls back to damped analog zeros; out-of-band zeros
  are omitted rather than aliased into the audible band. A second, stable
  minimum-phase SOS fits the smooth magnitude residual using 24 fixed samples
  in the φ basis, DC held exact and two numerator terms solved by weighted
  least squares. Its fixed poles are −0.4; corrections with near-unit zeros
  are rejected. Cuts are reciprocal boost designs and high shelves use the
  low-shelf complement. Passes use matched Butterworth sections; tilt uses
  matched first-order shelves. Double-state transposed direct-form-II runs
  every matched section. Frequency-dependent IIR phase remains intrinsic to
  the filter, without resampling group delay. Reported latency is zero.
  Zero-gain gain-type bands still bypass exactly.
- Coefficient fits use extended intermediates for low-frequency cancellation,
  then store double coefficients. Matched coefficients update every 16 samples
  while a smoother is moving (including its final grid update), with the grid
  retained across host blocks. Auto gain and response sweeps reuse already
  designed sections; unchanged curves do not trigger repeated auto-gain work.
- Accuracy was measured using `eq-bench --accuracy`, 25 centre frequencies
  from 10 Hz through 10 kHz, gains −30…+30 dB in 3 dB steps, Q 0.1…4 for
  bells / 0.1…1 for normal shelves. Each case probes 2049 log-spaced and 2049
  linearly spaced frequencies from 20 Hz to 20 kHz, plus centre/resonance probes.
  The engine tests require the same normal-domain ≤0.3 dB bound and independently
  compare processed sinusoids with the digital response. Worst measured errors:

  | Domain | 44.1 kHz | 48 kHz |
  |---|---:|---:|
  | Bell, normal domain | 0.173604 dB | 0.059654 dB |
  | Either shelf, normal domain | 0.254058 dB | 0.223204 dB |
  | Resonant shelves, f0 ≤10 kHz, Q 1.2…24 | 17.219869 dB | 0.840856 dB |
  | Bell near Nyquist, Q 4…24 | 1.500638 dB | 1.291749 dB |
  | Resonant shelves near Nyquist, Q 1.2…24 | 21.030647 dB | 16.180889 dB |

  The extra-domain sample set uses ±6/12/30 dB, centre frequencies 1/4/8/10 kHz
  for resonant shelves; near Nyquist uses 10/16 kHz and 0.45/0.49·fs. The large
  resonant-shelf errors occur at Q=24, where a pole/zero extremum is at or beyond
  the available bandwidth. They are explicit approximation limits, not part of
  the normal-domain guarantee. Complete worst-case coordinates are printed by
  the measurement executable and saved in the worker verification log.
- CPU budget: `eq-bench 120`, Clang `-O2`, 48 kHz, 512-frame stereo blocks,
  fixed non-flat HP/bell/high-shelf and eight-band mixed-filter patches,
  median of five CPU-time trials per variant (percent of one core):

  | Bands | Auto gain | Minimum | Natural | Natural/minimum |
  |---:|---|---:|---:|---:|
  | 4 | off | 0.3103% | 0.2440% | 0.787× |
  | 8 | off | 0.6081% | 0.4259% | 0.700× |
  | 4 | on | 0.3431% | 0.3138% | 0.915× |
  | 8 | on | 0.6675% | 0.4987% | 0.747× |

  This meets the ≤2× minimum budget. It is an offline benchmark executable,
  not a timing test or an audio-device test.
- All three phase choices are nonautomatable and require reactivation; latency
  remains fixed while active. Linear latency is round(2048·fs/48000); its
  128-sample convolution partition delay is included in that number. Symmetric
  Lanczos-windowed FIR kernels have 2·(latency−128)+1 taps. Narrow low-frequency
  responses have finite-length approximation limits. Linear kernel compilation
  is asynchronous and coalesces updates, followed by a 20 ms interpolation on
  the fixed partition grid; exact-offset automation applies to minimum/natural.
- Auto gain samples the summed affected-channel curve at 64 equally spaced
  log-frequency midpoints from 20 Hz to min(20 kHz,0.49·fs). It is a static
  pink-weighted estimate, bounded to ±24 dB compensation so deep stopbands
  cannot create unbounded gain, including for selective channel placement, not an
  input-dependent loudness match. Updates use a fixed 128-sample grid and a
  20 ms output ramp. The linear summed curve evaluates the actual windowed FIR;
  faint per-band curves show their design targets.
- Type/slope/channel changes fade out for 5 ms, clear filter state, then fade in
  for 10 ms; enables/disables use 10 ms ramps. Floats use 20 ms ramps in log
  frequency, dB gain and log Q.
- Shelf settling and pass −3 dB checks refer to the default Q (0.71); resonant
  user Q intentionally changes the response. Tilt uses symmetric first-order
  shelves around its frequency.
- Full-range stress uses low-level noise: simultaneous +30 dB boosts across
  eight bands legitimately exceed +40 dBFS on full-scale input. The EQ does not
  impose an undocumented limiter. Finite output and the +40 dBFS test bound are
  checked with −480 dBFS excitation throughout the full parameter domain in
  each phase mode, followed by two seconds of silence. Linear stress coalesces
  kernel publication every 4096 samples; phase itself is fixed per activation.
- The editor's pre/post spectrum uses core-analysis LTAS on 4096-sample batches
  copied as separate left/right channels through its telemetry ring; analysis runs on the GUI thread, never RT.
  Factory defaults include the high-pass, so the bit-exact null fixture disables
  it (or makes every band a zero-gain gain-type filter).

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
| `lookahead` | enum | `off`, `ms1`, `ms5` (latency) | | `off` |
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

Implementation decisions (2026-09-29):

- `rms_window` is the exponential mean-square time constant (one pole),
  not a rectangular window. The RMS sine curve uses input RMS dBFS, while
  the peak curve uses the detector sample level; a sine's peak and RMS differ
  by 3.0103 dB. Timing checks use a 20 dB constant-amplitude input step.
- Feedback detects the previous compressed wet sample before makeup, mix
  and output gain. An external sidechain always takes precedence over topology.
- Auto release blends the fast `release` and slow `5 * release` one-pole
  coefficients. Weight ramps from 0 to 1 over one second while a deeper
  reduction is held, and returns to 0 over one second when it is releasing.
  Auto makeup adds the negative static feedforward reduction at -18 dBFS
  to manual makeup, only in compressor mode.
- A limiter processes the makeup/mix/output-adjusted signal, then enforces
  its ceiling. Compressor threshold, detector, topology and sidechain do not
  alter limiter gain. `sc_listen` auditions the filtered key; in limiter mode
  the audition passes through the limiter too. Peak parameters in the GUI
  measure sample peaks; they are labeled dBFS.
- True-peak limiting adds 16 samples of latency whenever limiter mode and
  `true_peak` are selected, including lookahead `off`. With `off`, only the
  sample ceiling is guaranteed. Use `ms1` or `ms5` for the true-peak ceiling.
  Mode and true-peak changes are non-automatable because they can change latency,
  as is lookahead. All three configuration controls request a restart even
  when the resulting delay is equal, avoiding an unsmoothed topology switch.
  The active DSP retains the old latency configuration until
  the host reactivates; other parameters can continue to change.
- The limiter's minimum deque holds targets across the lookahead window;
  release is capped by the moving average, allowing the attack to ramp instead
  of jumping to the window minimum. The average has lookahead-samples + 1
  entries; the interpolator's 16-sample delay is excluded from that ramp.
  True-peak detection uses only the shared 4x interpolated driven signal,
  without an L1 reconstruction bound or additional detection margin. With TP
  lookahead the hold spans twice the total delay to cover the FIR support.
  Ceiling checks allow 0.1 dBTP measurement tolerance. A -3 dBFS sine below a
  -1 dBTP ceiling passes unchanged apart from delay; seeded pink noise driven
  +12 dB retains sample peaks within 0.6 dB of that ceiling. No lookahead
  setting is a loudness target.
- The design is 900 x 560, below Orrery's 1120 x 700 limit. Knob values sit
  below the knob and label, clear of the pointer. Limiter mode disables
  threshold, ratio, knee, attack, detector, RMS window, topology, auto release
  and auto makeup; key selection and filters are enabled only while key
  audition is on. Limiter key audition uses feedforward key filtering.
  Release, stereo link, makeup, mix, output, ceiling, true peak and lookahead
  remain active. Its transfer curve includes the makeup/dry-wet/output drive
  and flattens at the ceiling. Linux previews embed in a fixed-size parent
  so a tiling window manager cannot enlarge or distort the capture.
- Sidechain filters use 12 dB/oct Butterworth TPT sections. Endpoint bypass
  blends across 10–20 Hz for highpass and 19–20 kHz for lowpass; endpoints are
  exactly off. Continuous parameters smooth over 20 ms, positive log-domain
  controls in log space; ratio smooths its inverse in the gain-computer slope
  so the infinity endpoint has no jump. Detector/source/topology/listen
  switches use 10 ms ramps.

Lookahead choice keys are `ms1` and `ms5` (displayed as 1 ms and 5 ms):
core-params requires enum keys to start with a lowercase letter, so the
original `1ms` / `5ms` spellings could not pass table validation.

Mono main configurations retain a stereo key; its maximum detector level
controls the mono gain regardless of `stereo_link`. The implementation also
accepts positive sample rates through 768 kHz for host-validator coverage;
the response contract remains 22.05–384 kHz.

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

Implementation decisions (2026-09-30):

- Defaults for the abbreviated keys: `clip` false, `mix` 100 % (0..100),
  `output` 0 dB (-24..+24). All continuous controls smooth for 20 ms,
  speed in log space; restored values snap before processing.
- The stereo maximum rectifier feeds a cycle-aware rectangular peak hold:
  at least 1 ms, otherwise the interval between the two most recent local
  maxima plus one sample, capped at 50 ms. This suppresses carrier zero
  crossings so steady tones do not get constant onset gain. It adds no audio
  latency, but low-frequency decay detection can lag by up to half a cycle.
  A bounded preallocated monotonic deque implements the hold.
- Fast attack is `0.1 ms * speed/10`, fast release and slow attack are
  `speed`, slow release is `4*speed`; both operate in dB. At the first
  nonzero detector sample, both start 60 dB below that level. The silence
  floor tracks 120 dB below the largest detector level since reset. These
  relative references preserve identical gain curves under level scaling.
- With `d = fast - slow`, onset gain is
  `12*(attack/100)*clamp(d*sensitivity/500,0,1)`, sustain gain is
  `12*(sustain/100)*clamp(-d*sensitivity/500,0,1)`. Their sum feeds a 1 ms
  one-pole. Total gain is bounded to +/-12 dB because only one part is
  nonzero at a time. Sensitivity zero disables shaping. No threshold/key.
- Clip is `tanh`, unity small-signal slope, applied after shaping and before
  mix/output. The shared `dsp::Oversampler` at 2x uses a 145-tap Kaiser
  (beta 11) half-band pair with 72 base samples of round-trip delay. The dry
  path and reported latency follow the prepared oversampler's integer delay.
  Clip off has zero delay; `clip` is non-automatable and requests restart,
  retaining the old DSP and latency until reactivation. Output gain can raise
  clipped peaks; clip is a tone control, not a ceiling limiter.
  The alias regression uses a 5 kHz sine at amplitude 3 peak, 48 kHz sample
  rate: harmonics 7/9/11 fold to 13/3/7 kHz at 1x and must each be at least
  20 dB lower with 2x than base-rate tanh. Aliases between 20 kHz and
  Nyquist are accepted; the fifth harmonic's 23 kHz fold is outside this
  acceptance band.
- Appended readonly host meters: `input_level`, `output_level` (sample peak
  dBFS, -120..40), and `gain` (average shaping dB, -12..12), every 50 ms.
  The visible editor gets the same frames; its 900 x 560 design plots two
  seconds of envelopes plus gain. Knob values are below the face; attack,
  sustain and output arcs start at their zero centre.

## 5. Saturation — "Kiln" [`drive`]

Keys: `curve` enum `tape`, `tube`, `transistor`, `diode`, `fold`, `hard`
(`tape`); `drive` dB 0..36 (6); `bias` % −100..100 (0); `tone` dB/oct
−6..+6 (0); `oversampling` enum `x1`, `x2`, `x4`, `x8` (`x4`; displayed
1x/2x/4x/8x, stepped, latency); `auto_gain` bool (true); `mix` % 0..100
(100), `output` dB −24..+24 (0). Append-only readonly `input_level` and
`output_level` dBFS −120..40 (−120), sample peaks published every 50 ms.

Implementation decisions (2026-09-29; approved refinements to effects.md §7):

- Patch choice keys begin with a letter, as required by core-params:
  `x1`/`x2`/`x4`/`x8`. The original digit-leading keys were not valid.
- All curves have unity slope at zero and are odd and bounded. Define
  R(a,c,x) = a·(u + c·u³), u = (x/a)/sqrt(1+(x/a)²).
  Its even antiderivative, zero at the origin, is
  I(a,c,x) = a²·[(1+c)(r−1) + c(1/r−1)], r = sqrt(1+(x/a)²).
  Tape: R(1.2,0,x), F = I(1.2,0,x), wide soft knee.
  Tube: R(0.6,0.4,x), F = I(0.6,0.4,x), gentler onset but lower
  asymptotic ceiling (±0.84). Transistor: R(1,0.5,x), F = I(1,0.5,x),
  a rational-family knee with zero cubic term at the origin, bounded ±1.5.
  Diode: tanh(x), F = |x| + log(1+exp(−2|x|)) − log(2).
  Fold: 0.35·R(1.2,0,x) + 0.65·sin(x),
  F = 0.35·I(1.2,0,x) + 0.65·(1−cos(x)); the soft residual retains
  low-frequency content when the sine folds, so RMS compensation also
  meets the reference pink-noise LUFS tolerance. Hard: clip(x,−1,1),
  F = x²/2 for |x|≤1, else |x|−1/2. These are static character curves,
  not claimed physical circuit/tape models.
- First-order ADAA evaluates the antiderivative difference, falling back
  to f((x+x_previous)/2) for |difference|<1e-5. A half-sample averaging
  delay and factor−1 high-rate samples complete one integer base sample.
  The shared linear-phase half-band Oversampler uses 145/33/25 taps at
  2x/4x/8x, Kaiser beta 11, exact zero half-band taps. The base-adjacent
  stage has passband 0..0.45·fs and stopband from 0.55·fs; later stopbands
  start at 1.55·fs and 3.55·fs (fs is the base rate). Each stage rejects
  at least 100 dB; round-trip ripple is below 0.01 dB in 0..0.45·fs.
  Its round-trip delays are 0/72/80/83 samples; Kiln reports
  **1/73/81/84 samples**, with the same dry delay (1.688 ms at default
  4x/48 kHz). Symmetry and integer delay are preserved.
- Order: pre tilt → drive and bias/100 → ADAA curve → reference gain →
  integer delay padding → inverse tilt → 5 Hz DC blocker → downsample →
  latency-aligned mix → output. Tone is a first-order tilt at 1 kHz, with
  DC/HF gain −2·tone/+2·tone dB (canonical unit dB, UI explains dB/oct).
  Its slope varies with frequency, rather than being a straight line.
  The inverse exactly cancels the linear pre-filter. Floats ramp over
  20 ms; curve weights and auto-gain enable ramp over 10 ms. Values snap
  on reset/prepare; oversampling is adopted only on reactivation.
- `auto_gain` uses **Gaussian RMS**, superseding the small-signal model:
  σ = 10^(−18/20), g = 10^(drive/20), compensation
  σ/sqrt(E[f(g·σ·Z)²]), Z standard normal. Prepare computes a table for
  every curve at 0.25 dB drive intervals with 2048 trapezoids over Z∈[−8,8].
  Processing interpolates the table. Bias and tone are not compensated;
  this is not an input-adaptive loudness rider.
- THD is non-decreasing for tape/tube/transistor/diode/hard on a 1 kHz
  **−18 dBFS RMS** sine, drive 0/6/12/18/24/30/36, 4x, bias/tone zero,
  auto gain off. Fold is explicitly excluded because folding is
  non-monotonic. Bias 50% produces second harmonics above −40 dBc;
  bias zero leaves even harmonics below −100 dBc. DC mean after settling
  is below 1e-5. Small-signal alignment uses a 100 Hz, 0.0001 peak sine,
  tape, 0 dB drive, auto gain off and 50% mix (within 0.012 dB,
  including the specified 5 Hz DC blocker; 0.01 dB applies at default 4x).

Measured reference limits (Linux Clang dev; deterministic seed 481;
20 Hz–20 kHz FFT-shaped Gaussian pink noise, 65536 samples, 48 kHz;
startup discarded and dry/wet observation windows aligned; 4x, bias/tone
zero, auto gain on, drive 0/6/12/18/24/30/36):

| Curve | Largest absolute ΔLU at −18 dBFS RMS | ΔLU range at −30 dBFS RMS | ΔLU range at −6 dBFS RMS |
|---|---:|---:|---:|
| tape | 0.323 | 0..+9.966 | −11.120..0 |
| tube | 0.327 | 0..+10.758 | −11.355..0 |
| transistor | 0.320 | −0.109..+9.689 | −11.052..0 |
| diode | 0.343 | 0..+10.668 | −11.395..0 |
| fold | 0.906 | −0.033..+13.892 | −14.454..0 |
| hard | 0.351 | −0.120..+11.256 | −11.535..0 |

Aliasing: 5 kHz sine, **−18 dBFS RMS** (−14.9897 dBFS peak), 24 dB drive,
48 kHz, bias/tone zero, auto gain off; coherent 4800-sample FFT after 1 s
settling. Only unaliased 5/15 kHz harmonics are excluded; largest other
bin **within 20 Hz–20 kHz** relative to the fundamental. Aliases between
20 kHz and Nyquist are accepted; suppressing those inaudible components
is not a filter-design requirement:

| Curve | 4x (dBc) | 8x (dBc) |
|---|---:|---:|
| tape | −155.71 | −154.79 |
| tube | −104.89 | −149.90 |
| transistor | −134.76 | −158.25 |
| diode | −154.26 | −153.09 |
| fold | −159.59 | −158.53 |
| hard | −81.42 | −91.86 |

Smooth curves require ≤−90 dBc at 4x on this test; hard/fold require
≤−60 dBc at 8x and publish both measurements. Recommend **8x for hard/fold**.
These are level-specific measurements, not universal alias guarantees.

CPU measurement (Intel Core Ultra 5 225F, Linux Clang `-O2`, stereo 48 kHz,
256-frame blocks, 4x; median of three 10 s process-CPU intervals, excluding
prepare and 8192-frame warmup; 1 kHz sine at −18 dBFS RMS in each channel,
90° channel phase offset, auto gain on, bias/tone zero, mix 100%, output 0 dB):

| Curve / drive | CPU, percent of one core | ns per stereo frame |
|---|---:|---:|
| tape / 6 dB (default) | 1.883 | 392.3 |
| tape / 24 dB | 2.021 | 421.1 |
| tube / 24 dB | 1.953 | 406.9 |
| transistor / 24 dB | 2.031 | 423.0 |
| diode / 24 dB | 1.914 | 398.8 |
| fold / 24 dB | 2.246 | 467.9 |
| hard / 24 dB | 1.401 | 291.9 |

Reproduce with `cmake --build --preset dev -j6 --target drive-bench`, then
`build/dev/plugins/drive/engine/drive-bench`; the runner compiles both DSP
kernels with `-O2`. Numbers describe this workstation, not a CI threshold.
CI checks the same properties with shorter coherent observations, drive
0/18/36 and representative rates; per-sample checks accumulate a max/flag.
The opt-in `drive-engine.measure` target retains the dense drive/rate sweeps,
full reference-noise record, 1 s sine settling and 5 s automation stream;
it is not registered with CTest. The alias/loudness tables above come from
that runner.

GUI transfer/harmonics depict the static setting (sine spectrum computed
on the main thread), with programme peak telemetry for the moving dot.

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


### Delay implementation decisions (2026-09-30)

- Identity is `delay`, CLAP `com.suppleaudio.delay`, state `QDL1`, AU `QDly`;
  the display name is defined once in `plugins/delay/cmake/Identity.cmake`.
  Sync copies Orrery's `off`, `n8_1` … `n1_32` choices; `n1_8d` is default.
  Both line times clamp to 1–5000 ms after converting the note and applying
  the right offset to the original, unclamped left time. Missing, nonfinite
  or nonpositive transport tempo means 120 BPM. Output is −24…+24 dB (0),
  mix is 0…100 % (25), width is 0…200 % (100), freeze defaults off.
- A two-pole critically damped glide consists of cascaded one-poles with
  100 ms time constants, initialized to restored times before processing.
  This applies to free time, sync, offset and transport tempo. Other floats
  ramp for 20 ms (frequencies/rate in log space); routing modes and freeze
  use 10 ms ramps. Loop coefficients update on a fixed 16-sample grid.
  Frequencies clamp internally to 0.45·fs. Filters are always active;
  their range endpoints are wide settings, not bypasses.
- Float circular storage uses four-point cubic Hermite reads. Two lines are
  allocated for 5000 ms + 3 ms wow + interpolation margin. At validator-only
  rates below the 22.05 kHz response contract, reads have a two-sample causal
  minimum; activation accepts 1000…768000 Hz.
- Wow uses 0.8 sine + 0.2 smooth random, ±3 ms at full depth, capped to
  45 % of the current line time. The random endpoints come from fixed
  per-line xorshift32 seeds and join with a cubic smoothstep once per wow
  cycle. The right sine starts a quarter cycle later. Depth zero is exact.
- The safety curve is x for |x| ≤ 0.5, otherwise
  sign(x)·[0.5 + 0.5·tanh(2·(|x|−0.5))]. Every normal echo is bounded by
  unity before width/output. Drive zero bypasses its nonlinearity exactly;
  it crossfades linearly from bypass to tanh(g·x)/g over drive 0–1 dB,
  g = 10^(drive/20), using the full curve from 1 dB upward. This avoids a
  level step when leaving zero. Stereo, ping-pong and mono routing
  interpolate over 10 ms. Width 100 bypasses M/S arithmetic
  exactly; mono sends the left line to both wet outputs and ignores width.
- Duck measures the linked main input peak in dB with 1 ms attack and
  250 ms release; attenuation is duck·clamp((level+60)/30,0,1). It affects
  the wet outputs only, after recirculation, so it never changes the loop.
- Freeze holds the current unmodulated read times immediately; modulation,
  input and loop processing fade out over 10 ms. At completion each line
  replays a stationary view of its existing ring period, rounded to the
  nearest sample, with the held fractional phase interpolated only at output.
  The original samples are never rewritten. This avoids accumulating cubic
  interpolation loss on every frozen repeat; the held period differs by at
  most half a sample. No extra allocation, copy or filter reset occurs.
  Unfreeze aligns the write head with the frozen playback cursor and restores
  the normal loop over 10 ms. New time/tempo targets take effect on unfreeze.
- Tail is the longest requested line time × max(1,ceil(ln(1e-4)/ln(fb)))
  + 50 ms, rounded up to samples; zero feedback still has one repeat.
  Freeze or feedback ≥100 % reports INT32_MAX (infinite). **Approved
  correction to effects.md §10:** `clap_host_tail::changed` runs in
  `process()`, following CLAP's audio-thread contract; parameter/state edits
  set an atomic pending flag and notifications coalesce there. No restart
  is needed; latency is always zero.
- Nonfinite inputs become zero and finite inputs clamp to ±1e8 before the
  recursive loop. Recursive filter/storage states below 1e-30 flush to zero.
  The documented self-oscillation ceiling applies to each normal echo at
  width 100/output 0; width >100 or positive output gain can exceed 0 dBFS,
  and freeze intentionally bypasses the limiter.
- The 900×560 editor shows parameter/tempo-derived repeat positions and
  levels, a digital loop response, telemetry for wet level and ducking,
  and values below each knob. These are per-line nominal repeats, before
  output width/mix/duck; the fractional glide and wow alter actual timing.

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
