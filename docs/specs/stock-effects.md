# Spec: stock effects

**Status:** Implemented. Each plugin's section records its decisions;
`docs/specs/effects.md` holds the implementation notes.
**Common rules:** ADR-0010 (semantic params, value domains), ADR-0011
(template instantiation), ADR-0047 (stock plugins written as DSP scripts),
`docs/specs/core-params.md`, `core-patch.md`,
`semantic-params-extension.md`.

## 0. Conventions shared by every stock effect

- **Enum keys** start with a lowercase letter (core-params validation):
  numeric choices take a unit prefix — `db12`, `ms5`, `x4` — while their
  display labels keep the numbers ("12 dB/oct", "5 ms", "4x"). Tables below
  that still show digit-led keys mean these prefixed forms. A scripted stock
  plugin names them with `.keys({...})` (ADR-0047).
- **Patch form:** flat (core-patch `FlatPatchType`): `"params": {"<key>":
  value}` with dotted keys (`band2.freq`). One form for every effect, so an
  agent learns it once. Keys and units are the parameter table.
- **Names:** craft nouns; code identifiers use the functional id in
  brackets. CLAP ids `com.suppleaudio.<id>`.
- **Every effect:** mono/stereo configs (plus sidechain input where
  listed), float/double, sample-accurate automation, block-size invariant
  (null −120 dBFS across 1..4096), denormal-safe, deterministic (seeded where
  random), latency reported exactly (with compensated bypass), `mix`
  (dry/wet, latency-aligned) and `output` (dB) where meaningful, a
  `describe()` sentence, RT-safe under rtsan, CLAP validator clean, loads
  and automates in REAPER (`tools/reaper-check`).
- **Latency-changing parameters** (lookahead, oversampling, linear phase)
  are stepped, not automatable, and follow the template's restart contract
  (`request_restart`, reactivate with the new latency).
- **`reset()` clears processing state and keeps the parameter values** (CLAP's
  contract). The sandbox and the engine reuse one instance across renders and
  across seeks, so a render must depend only on the state set for it:
  smoothers snap to their targets on the first block after `reset()`,
  delay/time lines start at their value, and nothing of the previous render
  survives. `clap-reset-check` (`plugins/common/tools/clap_reset_check.cpp`,
  registered as `plugin.<id>.reset` for every stock plugin and Orrery) renders
  state S1, then S2 after `reset()`, and requires that S2 render to be
  bit-identical to a fresh instance holding S2 — at block 256 and again at 64.
  Scripted stock plugins (ADR-0047) also run it with `--stepped`
  (`plugin.<name>.reset-stepped`): every switch and choice moved off its
  default in both states, so a plugin whose default is a passthrough still
  shows its processing state.
- **Readonly telemetry parameters** (gain reduction, levels) exist for
  generic hosts; the GUI uses the telemetry ring.
- **Tests beyond the template's:** frequency/phase response against analytic
  targets, THD/aliasing measurements where nonlinear, envelope timing
  against spec, sidechain behaviour, parameter-sweep automation stress, and
  golden renders (tolerance-aware null) for every factory preset.

## 1. EQ — "Contour" [`contour`]

An eight-band equaliser with zero latency, written as a DSP script
(ADR-0047: `plugins/stock/contour/script/contour.cpp`). Built for the
analysis-driven workflow ("cut 250 Hz by 3 dB, Q 1.2" is one merge patch)
and for hands: dynamic bands, mid/side placement, band listening, gain
scaling, and a spectrum analyser that favours bass resolution.

| key | type | range | curve | default |
|---|---|---|---|---|
| `bandN.enabled` (N = 1..8) | bool | | | off (a fresh instance is flat) |
| `bandN.type` | enum | `bell`, `low_shelf`, `high_shelf`, `low_cut`, `high_cut`, `notch`, `tilt` | | 1 `low_cut`, 2 `low_shelf`, 3–6 `bell`, 7 `high_shelf`, 8 `high_cut` |
| `bandN.freq` | Hz | 20..20000 (designed at most 0.45 fs) | log | 30, 100, 250, 800, 2500, 6000, 10000, 18000 |
| `bandN.gain` | dB | −30..+30 (bells, shelves, tilt) | linear | 0 |
| `bandN.q` | none | 0.1..30 (shelves use 0.25..2, tilt 0.25..1) | log | 0.71 (shelves, cuts), 1 (bells) |
| `bandN.slope` | enum | `db6`, `db12`, `db18`, `db24`, `db36`, `db48` (cuts) | | `db24` |
| `bandN.channel` | enum | `stereo`, `mid`, `side`, `left`, `right` | | `stereo` |
| `bandN.dynamic` | bool | bells and shelves only | | off |
| `bandN.threshold` | dB | −60..0 | linear | −30 |
| `scale` | % | 0..200: every bell, shelf and tilt gain times this | linear | 100 |
| `auto_gain` | bool | output offset by the static curve's average effect | | off |
| `output` | dB | −24..+24, smoothed 20 ms | linear | 0 |
| `listen` | int | 0..8: plays only band N's region; not automatable | | 0 |

DSP: every section is a second-order (or first-order) filter in transposed
direct form II, in double precision. Poles sit where the analog prototype's
are (impulse invariance); zeros are fitted to the prototype's magnitude,
exact at DC and at the band's frequency (a shelf's zero frequency below
0.6·Nyquist), with the Nyquist term chosen by least squares of the relative
power error at 16 log-spaced frequencies from a quarter of that frequency
up. Bells use the RBJ analog prototype (boost and cut mirror each other); a
cut, and a shelf designed past its corner, is the opposite design inverted,
so poles always lie below the corner. Cuts are matched Butterworth
cascades (odd orders end with a matched first-order section); `q` scales
the most resonant section (0.71 is Butterworth). Notch: poles from `q`,
zeros on the unit circle, unity gain at DC. Tilt: low shelf −gain/2 and
high shelf +gain/2 at `freq`. A band's section count is fixed by its type
and slope, and a gain type at 0 dB is sections that pass the input exactly.
Reported latency is zero. Measured against the analog prototypes through
20 kHz at 44.1 and 48 kHz (25 centres 10 Hz–10 kHz, gains in 3 dB steps,
2049 log-spaced probes): bells with Q 0.3..4 within 0.35 dB at ±12 dB and
0.54 dB at ±30 dB; shelves (Q 0.25..1) within 0.19 dB at ±12 dB and 0.42 dB
at ±30 dB. Wide bells above 5 kHz are the worst case; resonant shelves are
limited to Q 2, where the fit stays within 0.95 dB at ±30 dB.

Every 16 samples, on a grid kept across host blocks, frequencies glide in
log frequency, gains in dB and Q in log over 25 ms, and a band redesigns
only while something moves. Turning a band on or off, or changing its type,
slope or channel, fades the band out over 12 ms, switches and clears its
state, and fades it back in.

Dynamic bands follow the level of their own region at their input (a
band-pass at the band's frequency and Q for bells, the low- or high-pass
side of a shelf), detected per channel as the band is placed (both stereo
channels linked), with a 3 ms attack and a 90 ms release (both longer
below 100 and 200 Hz). Above the threshold the band moves toward its gain
by three quarters of the overshoot, with a 6 dB soft knee, never past its
gain; below it the band rests at 0 dB.

Auto gain offsets the output by the negative of the curve's mean power gain
across 30 Hz–16 kHz (40 log-spaced points, weighted toward the midrange by
two second-order roll-offs at 90 Hz and 7 kHz), with dynamic bands at rest,
within ±18 dB, gliding over 150 ms. It is a static estimate, not a loudness
match.

Listen plays the band's region: a band-pass at its frequency and Q for
bells, notches and tilts, the low-pass side for low shelves and low cuts
(what a low cut removes), the high-pass side for high shelves and high cuts,
faded in and out over 12 ms.

Editor (1000 × 624, the script's own drawing): the response graph over
most of the window, log 20 Hz–20 kHz with a ±6/12/24/30 dB scale; the summed
curve (every band that is on, whatever channel it works on, plus the
output gain) over the output's spectrum; numbered nodes for the eight bands
(switched-off bands faint, still draggable); the selected band shaded; a
dynamic band drawn at full reach with a ring where its gain is now; a
channel badge under mid, side, left and right bands; input and output peak
meters; a panel for the selected band (switch, listen, type, channel,
frequency, gain, Q, slope, dynamics with a live detector meter and
threshold) and one for the output (gain scale, output, auto gain, peak).
Drag a node for frequency and gain (Q for cuts and notches); wheel for Q
(Alt: slope for cuts); Shift for fine moves; Ctrl snaps to semitones (A4 =
440 Hz); Alt-drag listens to the band; double-click a node to switch it off,
empty graph to add a band there (a cut below 32 Hz and above 15 kHz, a bell
between). Hovering reads out frequency, note and cents.

The analyser runs in the editor, never on the audio thread: the audio code
keeps the newest 32768 samples of the output (mono) in a shared ring; at
most every 40 ms of new audio the editor transforms them (Hann window,
1.46 Hz bins at 48 kHz), smooths each of 420 log-spaced points over a
sixteenth of an octave where bins are dense and interpolates in dB (cubic)
where they are sparse, averages power over about half a second, rounds peaks
with a five-point binomial kernel across neighbouring points, tilts 4.5 dB
per octave about 1 kHz, and eases the drawn line toward that (rising with a
50 ms and falling with a 120 ms time constant). It is drawn as one smooth
line (Catmull-Rom through the points), unfilled, from −96 dB at the floor to
0 dB at the top; silence lies along the floor.

Factory patches: Flat, Clean Up, Vocal Polish (auto gain), De-Ess, Air and
Weight (auto gain), Mono Bass (side cut), Telephone.

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
  alter limiter gain. `sc_listen` auditions the filtered key at the reported
  latency, for internal/external keys and audition/key-source transitions;
  its delay storage is prepared before processing. In limiter mode the audition
  passes through the limiter too. Peak parameters in the GUI
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

### Reverb implementation decisions (2026-09-30)

- Identity follows the compressor: `plugins/reverb/cmake/Identity.cmake`
  defines `REVERB_DISPLAY_NAME`; the engine receives
  `SUPPLE_REVERB_DISPLAY_NAME`, and all artifact paths use the CMake variable.
  CLAP id `com.suppleaudio.reverb`, state `QRV1`, AU subtype `QRvb` remain
  functional identities. Decay multipliers use core unit `x`, with text such
  as `1.5 x` accepted by patches and CLAP. Mono/stereo, float/double, zero latency;
  predelay is a musical delay, not reported latency. The dry null uses mix 0;
  defaults intentionally contain wet audio.
- Structure: 12 early taps per input channel, then four Schroeder allpasses
  per channel into a 16-line normalized Hadamard FDN. A private original LCG
  (`1664525*s + 1013904223`, unsigned 32-bit wrap) starts at `0x4e415645`.
  Line i's base milliseconds are `25*(95/25)^(i/15)*(0.95+0.1*u)`.
  Subsequent seeded early times are `5+75*(j+0.6*u)/12` ms; gains are
  `exp(-time/35)`, normalized to sum to 1 per input. Each tap alternates
  80/20 and 20/80 output pan (the other input reverses it). Allpass times
  are `(1+11*j/3)*(0.9+0.2*u)` ms. Size multiplies all times by
  `2^((size-50)*0.03)`. Stationary lengths round to samples; early taps and
  diffusers glide through cubic Hermite reads when size moves.
- The feedback transform is the 16-point Walsh–Hadamard butterfly scaled
  by 1/4. Input sign vectors are Hadamard columns 8 and 4, injected at 1/4;
  output sign vectors are columns 1 and 2, scaled by 1/sqrt(8). They are
  mutually orthogonal. Width acts on the combined early/late wet signal;
  mono output takes its mid. There is no limiter or automatic level match.
- Approved correction: decay shelves are second-order Butterworth-Q
  (1/sqrt(2)) sections rather than the plan's first-order shelves. The latter
  gave 11.7572% worst RT60 error in the requested matrix. Each line's mid
  attenuation is `10^(-3*L/(fs*T))`, with DC/Nyquist shelf gains derived from
  the low/high RT60 ratios. Geometric analog pole/zero placement holds the
  shelf midpoint at its frequency; the bilinear transform preserves the
  endpoints. Double-state transposed direct-form-II runs the sections, with
  exact bypass for flat shelves. Coefficients update on a fixed 16-sample
  grid while size/decay/filter controls move, including the final update;
  mix, output, width and texture changes do not redesign decay filters.
- Diffuser coefficient is `0.7*diffusion/100`, capped at
  `10^(-3*longest_allpass_seconds/(0.5*shortest_band_RT60))`. Without this
  cap, input diffusion itself outlasted short high-frequency tails at large
  size (40.87% error in the expanded matrix). Defaults/hall retain the
  planned coefficient. This cap keeps diffuser RT60 at most half the shortest
  requested band tail. `early` uses finite -60 dB with `kOffAtMin`, exactly
  off at the endpoint; all other values retain their finite dB gain.
- Fractional FDN reads use an original twelve-point centred Lagrange basis,
  precomputed at activation into 257 phases and linearly blended between
  adjacent rows. Integer positions bypass interpolation exactly. This
  replaces cubic FDN interpolation: with default modulation, size 20,
  decay 8 s and damping 2 kHz, cubic gave 8 kHz T30 RT60 3.277741/3.362107 s
  at 44.1/48 kHz against a 4 s target. The twelve-point read gives
  3.945919/3.949229 s (1.35/1.27% error). It also serves size glides, avoiding
  an interpolation-mode switch. Predelay, early taps and diffusers retain
  cubic reads; predelay below one sample uses a causal linear read.
- Each modulation oscillator has rate
  `0.1+1.1*(i+0.7*u)/16` Hz and phase `2*pi*u`, from seed `0x53504143`.
  A double sine/cosine rotation runs per sample; depth is at most 0.5 ms.
  Zero modulation is exactly zero offset. All buffers are preallocated
  powers of two, with double storage and recursive states flushed below
  1e-30. RT processing has no allocation or synchronization.
- Controls have finite 20 ms physical ramps; output gain ramps in amplitude.
  Delay positions use two cascaded one-pole smoothers with 50 ms time
  constants (critically damped); initial/restored values snap before audio.
  Frequency targets clamp to 0.45*fs internally. The response contract is
  22.05–384 kHz; activation also accepts 1–768000 Hz for host-validation
  coverage, with seven-sample minimum FDN lengths and bounded shelf ratios
  protecting unusually low rates.
- Freeze fades wet excitation out in 10 ms, moves loop attenuation to unity,
  removes modulation, and holds size. Held line lengths settle on the nearest
  integer sample; full freeze bypasses the damping sections and recirculates
  through the orthogonal matrix. This avoids interpolation loss in a held
  tail. After unfreezing, the latest size target glides into place. The
  measured stereo tail RMS change over the ten-second comparison is
  -0.0326 dB. Dry input still passes according to mix.
- Tail extension correction: `clap_host_tail::changed` is an **audio-thread**
  callback. Main-thread edits/state loads set an atomic pending flag;
  `process()` adopts values and notifies the host. Freeze reports UINT32_MAX;
  otherwise tail samples are ceil(fs/1000 * (predelay + 1.5*longest_RT60_ms
  + 100)). Tail queries are safe on either permitted thread. No restart is
  required. A product-owned test checks state edits, inactive flush,
  sample-offset freeze automation and callback thread placement.
- RT60 measurements use stereo octave-filtered impulse energies (fourth-order
  Butterworth edges), Schroeder backward integration, and a least-squares
  -5..-35 dB T30 fit. The matrix is 44.1/48/88.2/96/192 kHz,
  decay 0.5/2/8 s, size 20/50/90; both the default decay shape and
  low_decay 1.5 / crossover 1 kHz / damping 2 kHz. High_decay is 0.5,
  modulation and early taps off for these static measurements. Bands at least
  an octave from both transition frequencies are compared with their region
  target; other bands are measured but excluded from the plateau bound.

  | Octave band | Worst absolute relative RT60 error |
  |---:|---:|
  | 125 Hz | 7.4396% |
  | 250 Hz | 4.3463% |
  | 500 Hz | 7.4759% |
  | 1 kHz | 6.0268% |
  | 2 kHz | 7.6802% |
  | 4 kHz | 6.3230% |
  | 8 kHz | 2.7007% |

  The normal matrix meets 10%; default-modulation long/bright-tail coverage
  is the separate 8 kHz measurement above. Extreme ratios, overlapping
  transitions and the 100 ms/maximum-size corner remain approximation limits,
  outside this measured matrix. Input cuts change excitation, not loop RT60.
- Echo density follows the statistical definition of
  [Abel and Huang (2006)](https://aes.org/publications/elibrary-page/?id=13819):
  the fraction of samples outside the local standard deviation, divided by
  erfc(1/sqrt(2)). Tests use a centred 20 ms rectangular window, 1 ms steps,
  and a crossing sustained for 10 ms: default 67 ms, diffusion 100 at 52 ms.
  Late-tail correlation at width 100 is 0.049305; width 0 renders identical
  channels. Commands and CPU numbers are in `plugins/reverb/VERIFICATION.md`.
  The engine test requires density >=0.8 by 150 ms, earlier with diffusion 100,
  late-tail correlation magnitude <0.3 at width 100 and >0.99 at width 0.
- Test-cost correction after review: `reverb-engine.measure` is an opt-in,
  unregistered, `EXCLUDE_FROM_ALL` executable. It retains the full 90-response
  RT60 matrix, seven bands, original density/stereo windows and both long
  modulated-decay measurements above. Routine CTest uses three 48 kHz cases:
  decay 0.5/2 s at size 50 (flat low shelf), and decay 0.5 s at size 90 with
  low_decay 1.5/crossover 1 kHz; damping 2 kHz, bands 250 Hz / 1 kHz / 4 kHz
  outside active transitions. IR duration is 1.2 times the longest RT60 plus
  200 ms, leaving the truncation floor below the T30 fit. Density/stereo use
  1.25 s IRs, and the modulated 8 kHz check uses decay 2 s at 48 kHz.
  All sample-level predicates are aggregated without dropping samples; the
  ten-second freeze comparison and five-second automation stress are retained.
  The TSan-specific timeout override is removed, not increased. Timings and
  runner commands are in `plugins/reverb/VERIFICATION.md`.
- The editor is 900 x 560, with the small Supple Audio header, values below
  knobs, a bipolar output arc, decay target/input-bandwidth curves and gated
  sample-peak telemetry. Linux previews use a fixed embedded parent so a
  tiling window manager cannot resize the capture. CPU uses a standalone
  `reverb-bench` executable, Clang -O2, 48 kHz stereo 512-frame blocks,
  120 audio seconds per trial, median of five CPU-time trials with the editor
  closed; timing is never a CTest pass/fail condition.

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
- Non-finite working inputs are zero before DC/side-filter state updates.
  Exact unity output preserves the original bits, including non-finite samples,
  signed zero and subnormals, without contaminating that state.
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

## 10. Acceptance

A plugin is done when its tests, the CLAP validator, the RT sanitizer run,
a REAPER load and automation check, and its skill page exist.
