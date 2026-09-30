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

## 4. Transient shaper — "Flint" [`transient`]

Keys: `attack` % −100..+100 (0; boost/cut of the transient portion, ±12 dB
at the extremes), `sustain` % −100..+100 (0), `speed` ms 1..50 log (10;
detector time constant separating attack from sustain), `sensitivity` %
0..100 (50), `clip` bool (soft clip output), `mix` %, `output` dB. DSP: dual
envelope followers (fast/slow) on a detector signal; gain = f(fast − slow);
level-independent (a 10 dB louder input yields the same shaping).

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

## 10. Order of work and acceptance

After Orrery reaches its evaluation milestone: EQ → compressor/limiter →
utility + analyzer (needed for mixing tasks) → saturation → gate →
transient → delay → reverb. Each plugin is done when its tests, the CLAP
validator, the RT sanitizer run, a REAPER load/automation check (Nate or an
agent via computer-use), and its skill page exist.
