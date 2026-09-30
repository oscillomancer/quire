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
  mix/output. The shared oversampler had not landed at implementation start;
  the local 2x pair uses 257-tap Kaiser (beta 11) windowed-sinc half-band FIRs,
  128 high-rate samples of group delay per FIR, 128 base samples total.
  The dry path has the exact same integer delay. Clip off has zero delay;
  `clip` is non-automatable and requests restart, retaining the old DSP and
  latency until reactivation. Output gain can raise clipped peaks; clip is
  a tone control, not a ceiling limiter.
- Appended readonly host meters: `input_level`, `output_level` (sample peak
  dBFS, -120..40), and `gain` (average shaping dB, -12..12), every 50 ms.
  The visible editor gets the same frames; its 900 x 560 design plots two
  seconds of envelopes plus gain. Knob values are below the face; attack,
  sustain and output arcs start at their zero centre.

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
