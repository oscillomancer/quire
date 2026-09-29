# Stock effects, first wave: Contour, Press, Margin, Trim

**Status:** Draft for implementation, 2026-09-29. Build prompt Phase 3 (EQ,
dynamics, utility) — the plugins an agent needs for corrective mixing and
loudness delivery, and the "defined benchmark load" of the Phase 4 B1
benchmark. Builds on ADR-0010 (semantic parameters), ADR-0011 (plugins from
the template), ADR-0018 (sidechain inputs). Saturation, delay, reverb, gate,
transient shaper and the live analyzer follow in a second wave with the same
shape.

| Plugin | Role | CLAP id | State magic | AU subtype | Features |
|---|---|---|---|---|---|
| **Contour** | parametric EQ | `com.suppleaudio.contour` | `QCT1` | `QCtr` | audio-effect, equalizer, stereo |
| **Press** | compressor (sidechain) | `com.suppleaudio.press` | `QPS1` | `QPrs` | audio-effect, compressor, stereo |
| **Margin** | true-peak limiter | `com.suppleaudio.margin` | `QMG1` | `QMrg` | audio-effect, limiter, mastering, stereo |
| **Trim** | utility | `com.suppleaudio.trim` | `QTR1` | `QTrm` | audio-effect, utility, stereo |

Names are ours to change cheaply before release (HUMAN_REVIEW); ids and
magics are permanent once a session has saved one.

## 1. The shape every effect shares

### 1.1 Layout

Like Orrery (`plugins/orrery`):

- `plugins/<name>/engine/` — the SDK-free core, a monorepo library
  `supple::<name>-engine` (namespace `supple::<name>`), built and tested by the
  monorepo with its warnings and all sanitizers (label `rt` on its tests):
  - `include/<name>/params.h` — the one `core-params` table (`ParamSpec`s:
    permanent CLAP ids, keys, units, ranges, curves, tags, descriptions) and
    the `patch::FlatPatchType` (`"supple.<name>-patch/1.0"`, no seed);
  - `include/<name>/processor.h` + `src/` — the DSP: `prepare(sampleRate,
    maxFrames)`, `reset()`, `setParam(index, physical)` (RT), `process(in, out,
    frames[, key])` (RT, `SUPPLE_RT`), `latency()`, plus pure functions the
    GUI and tests use (e.g. Contour's magnitude response);
  - `describe()` — the compact prose (§1.6);
  - `tests/` — the monorepo tests of §1.7.
- `plugins/<name>/` — a copy of the template made exactly as
  `plugins/probe-synth` was (read its `QUIRE.md`: identity renames, vendored
  dependency paths, `supple_plugin_add_monorepo_libraries()`, format-gate and
  license-check handling, `supple_add_plugin(<name> <PREFIX> <Name>)` in
  `plugins/CMakeLists.txt`). The template is an effect already: keep its
  audio-port and process structure. Its `*_clap.cpp` product sections forward
  to `supple::clapx::ParamStore` / `ClapParams` / `FlatSemantic`
  (`plugins/common`) and to the engine; `QUIRE.md` lists every deviation.
  The shared template infrastructure stays byte-identical apart from names.
- The template's DSP header (`dsp/include/<name>/Processor.h`) is replaced by
  the engine library, as in Orrery.

Do **not** modify `~/Projects/audio-plugins/template`, `plugins/orrery`,
`plugins/probe-synth` or another effect's directory. Shared DSP primitives
that two plugins need go to `libs/core-dsp` only when the brief says so.

### 1.2 Parameters and state

- Keys are `section.name` in snake case (`band3.freq`, `key.hpf`); groups
  mirror sections. Units are canonical (`Hz`, `dB`, `ms`, `percent`,
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
- Denormals: `dsp::DenormalGuard` around processing; recursive states are
  also flushed to zero below 1e-30 when the input has been silent.
- Parameter changes are smoothed (no zipper noise); a switch that cannot be
  smoothed (a filter type, a detector mode) crossfades over 10 ms.
- Mono (1-in/1-out) works where the template supports it; stereo is the
  default configuration.
- Latency is reported through `clap.latency` and changes only when a
  parameter that defines it changes (reported the way the template reports a
  latency change); it is constant for a given parameter state.

### 1.4 Determinism

No randomness. The same state, input and events give the same output across
runs, processes and sample-rate-independent block partitions (§1.3).

### 1.5 GUI

Product-owned `Editor.cpp`/`Theme.h`/`Widgets.h` usage (the template's widgets
and window stay as shipped). Each plugin's layout is in its section below;
all read names, units and formatted values from the table, and every edit is
a gesture-bracketed parameter change. `tools/gui_preview` renders at least
two scenarios per plugin to PNG (the orchestrator reviews them).
Design size ≤ 900 × 560. The look follows the product theme used by Orrery
(`plugins/orrery/plugin/gui/Theme.h`): same palette and type, so the stock
plugins read as one family.

### 1.6 describe()

At most 300 characters for `changedOnly`, listing only what is not at its
default, in signal-flow order, with units — e.g. Contour: `low cut 80 Hz
24 dB/oct → bell 250 Hz −3 dB Q 1.2 (mid) → high shelf 10 kHz +2 dB; output
−1 dB`. Full detail may list everything.

### 1.7 Tests

In the engine library (monorepo, all presets):

- table validation, patch round trip (defaults and a full non-default patch),
  merge errors with paths;
- **null test:** default parameters pass audio through bit-exactly (Contour,
  Trim) or within the plugin's stated tolerance (Press below threshold,
  Margin below ceiling: delayed by the latency, else unchanged within
  1e-6);
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
(`plugin.<name>.validator`, label `slow`).

### 1.8 Skill and docs

- `skill/guides/<name>.md`: what the plugin is for, every key with its unit
  and range, three short recipes (e.g. Contour: remove rumble, carve a
  masking region reported by `analyze.masking`, de-harsh), and honest limits.
  Linked from `skill/SKILL.md` and `skill/guides/mixing.md`.
- `plugins/<name>/QUIRE.md` (deviations from the template) and a STATUS row.

## 2. Contour — parametric EQ

### 2.1 Parameters

Globals:

| key | type/unit | range | default | notes |
|---|---|---|---|---|
| `output.gain` | dB | −24 … +24 | 0 | |
| `output.scale` | percent | 0 … 200 | 100 | scales every band's gain (and tilt) — "halve the EQ" |

Eight bands `band1` … `band8`, each:

| key | type/unit | range | default | notes |
|---|---|---|---|---|
| `bandN.enabled` | bool | | false | |
| `bandN.type` | enum | `bell`, `low_shelf`, `high_shelf`, `low_cut`, `high_cut`, `notch`, `band_pass`, `tilt` | bell | |
| `bandN.freq` | Hz, log | 10 … 22 000 | 50, 100, 250, 500, 1000, 2500, 5000, 10 000 | |
| `bandN.gain` | dB | −30 … +30 | 0 | bell, shelves, tilt |
| `bandN.q` | none, log | 0.1 … 30 | 0.71 | bell/notch/band-pass width; shelf and 12 dB cut resonance |
| `bandN.slope` | enum | `db6`, `db12`, `db18`, `db24`, `db36`, `db48` | db12 | cuts only |
| `bandN.channels` | enum | `stereo`, `mid`, `side`, `left`, `right` | stereo | |

A band that is disabled, or whose gain is 0 dB for a gain-type filter, costs
nothing and passes audio bit-exactly.

### 2.2 DSP

Serial bands in index order. Each second-order section is a
trapezoidal-integrated state-variable filter (Simper/Cytomic "linear
trapezoidal SVF"): per channel state `ic1eq, ic2eq`,

    g = tan(π·f/fs), k = 1/Q, a1 = 1/(1 + g(g + k)), a2 = g·a1, a3 = g·a2
    v3 = v0 − ic2eq; v1 = a1·ic1eq + a2·v3; v2 = ic2eq + a2·ic1eq + a3·v3
    ic1eq = 2·v1 − ic1eq; ic2eq = 2·v2 − ic2eq
    y = m0·v0 + m1·v1 + m2·v2

with (A = 10^(gain/40)):

| type | g | k | m0 | m1 | m2 |
|---|---|---|---|---|---|
| bell | tan(πf/fs) | 1/(Q·A) | 1 | k(A² − 1) | 0 |
| low shelf | tan(πf/fs)/√A | 1/Q | 1 | k(A − 1) | A² − 1 |
| high shelf | tan(πf/fs)·√A | 1/Q | A² | k(1 − A)·A | 1 − A² |
| notch | tan(πf/fs) | 1/Q | 1 | −k | 0 |
| band-pass (0 dB peak) | tan(πf/fs) | 1/Q | 0 | k | 0 |
| low-pass section | tan(πf/fs) | 1/Q | 0 | 0 | 1 |
| high-pass section | tan(πf/fs) | 1/Q | 1 | −k | −1 |

- Cuts: Butterworth cascades — `db6` one first-order TPT section; `db12` one
  section at Q = `bandN.q` (0.71 = Butterworth); `db18` first-order + Q 1.0;
  `db24` Q 0.5412, 1.3066; `db36` Q 0.5176, 0.7071, 1.9319; `db48` Q 0.5098,
  0.6013, 0.9000, 2.5629.
- Tilt: a first-order low shelf of −gain/2 and a first-order high shelf of
  +gain/2 at `freq` (total tilt `gain` dB from lows to highs).
- Gains are multiplied by `output.scale/100` before design.
- Mid/side bands encode M = (L+R)/2, S = (L−R)/2 around that band only and
  decode after it; left/right bands leave the other channel untouched.
- Smoothing: `freq` (in log2), `gain` (dB) and `q` (log) are smoothed per
  sample (20 ms); coefficients are recomputed per sample only while a band's
  smoother is moving. Enabling/disabling a band or changing its type or slope
  crossfades the band's output with its input over 10 ms (type change: fade
  out, switch and clear state, fade in).
- Latency 0.
- `double magnitudeDb(const Values&, double hz, double fs)` evaluates the
  exact response of the digital cascade (from each section's equivalent
  biquad) for the GUI curve and tests; `channels` other than stereo are
  reported for the channel they affect.

### 2.3 Behaviour tests

- a bell at 1 kHz, +6 dB, Q 1: +6.0 ± 0.05 dB at 1 kHz; the −3 dB-of-gain
  points match the analog prototype within 5 % at 48 kHz;
- shelves reach their gain within 0.1 dB two octaves past `freq`;
- cuts: −3.0 ± 0.1 dB at `freq` (Butterworth slopes) and the stated slope
  (±1 dB/oct) two to three octaves out;
- measured responses (sine sweeps through `process`) match `magnitudeDb`
  within 0.05 dB at 20 frequencies, at every sample rate of the sweep;
- mid band on a mono-compatible signal (L = R) leaves the side at zero;
  side band on L = R input changes nothing;
- `output.scale` 50 halves every band's dB gain (measured).

### 2.4 GUI

A response display over most of the window: log frequency 20 Hz–20 kHz,
±24 dB (±12 dB zoom toggle), the summed curve plus a faint curve per enabled
band, numbered band handles (drag = freq/gain, wheel = Q, double-click =
enable/disable, right-click = type menu). Below it a strip of eight band
columns (enabled, type, freq, gain, Q, slope when a cut, channels) and the
output gain/scale. Scenarios for `gui_preview`: `flat`, `mix` (low cut,
bell cut, high shelf, a side band).

## 3. Press — compressor with sidechain

### 3.1 Ports

Main stereo input/output plus a second, non-main stereo input port named
"Sidechain" (`CLAP_PORT_STEREO`, not `CLAP_AUDIO_PORT_IS_MAIN`), per ADR-0018.
When `key.source` is `external` and the host gives no key (or silence), the
detector hears silence (no gain reduction).

### 3.2 Parameters

| key | type/unit | range | default | notes |
|---|---|---|---|---|
| `comp.threshold` | dB | −60 … 0 | −18 | |
| `comp.ratio` | ratio, log | 1 … 20 | 4 | |
| `comp.knee` | dB | 0 … 24 | 6 | full knee width |
| `comp.attack` | ms, log | 0.05 … 300 | 10 | |
| `comp.release` | ms, log | 5 … 3000 | 120 | |
| `comp.detector` | enum | `peak`, `rms` | peak | RMS: 10 ms mean-square window before the ballistics |
| `comp.lookahead` | ms | 0 … 10 | 0 | latency |
| `comp.link` | percent | 0 … 100 | 100 | stereo link of the detector |
| `key.source` | enum | `internal`, `external` | internal | |
| `key.hpf` | Hz, log | 20 … 1000 | 20 | detector high-pass (12 dB/oct); 20 = off (`kOffAtMin`) |
| `key.listen` | bool | | false | outputs the detector signal (after the HPF) |
| `output.makeup` | dB | −12 … +36 | 0 | |
| `output.mix` | percent | 0 … 100 | 100 | parallel compression (dry is latency-aligned) |
| `meter.gr` | dB, read-only | −60 … 0 | 0 | current gain reduction |

### 3.3 DSP

- Detector input: the key (internal: the main input; external: the sidechain
  port), high-passed by `key.hpf`; per channel level `x` (peak: |s|; rms:
  √(one-pole mean square, τ = 10 ms)); link: `d = link·max(x_L, x_R) +
  (1 − link)·x_c` per channel.
- Gain computer in dB (Giannoulis/Massberg/Reiss 2012, soft knee W):
  below T − W/2: 0; above T + W/2: (1/R − 1)(x − T); inside:
  (1/R − 1)(x − T + W/2)² / (2W).
- Ballistics on the gain reduction in dB: smooth branching one-pole —
  attack coefficient when the target reduction is deeper than the current,
  release otherwise; α = exp(−1/(t·fs)).
- Lookahead: the audio path (and the dry path of `mix`) is delayed by
  `lookahead` samples; the detector is not. Latency = lookahead samples.
- Output = mix·(delayed input · gain · makeup) + (1 − mix)·delayed input.
- `meter.gr` publishes the deepest reduction since the last report.

### 3.4 Behaviour tests

- static curve: steady sines at levels from −60 to 0 dBFS give the gain
  computer's output within 0.2 dB (after settling), knee included;
- attack/release: after a 20 dB step the reduction covers 63 % of the change
  in `attack` (deeper) or `release` (back up), ±10 %;
- external key: a silent main input keyed by a loud key → zero output change
  without makeup; a loud main keyed by silence → no reduction;
- lookahead: a transient's reduction starts `lookahead` earlier relative to
  the delayed audio; reported latency equals the delay;
- `key.hpf` 200 Hz: a 50 Hz key barely triggers (< 1 dB) where a 1 kHz key at
  the same level reduces fully.

### 3.5 GUI

Transfer curve (input vs output dB with the knee, a moving dot for the
current level), a gain-reduction history strip (the last ~4 s), the
parameter controls in three groups (compressor, key, output), and a GR meter.
Scenarios: `default`, `heavy` (ratio 10, knee 0, external key).

## 4. Margin — true-peak limiter

### 4.1 Parameters

| key | type/unit | range | default | notes |
|---|---|---|---|---|
| `limit.input` | dB | 0 … +24 | 0 | drive into the limiter |
| `limit.ceiling` | dB | −12 … 0 | −1 | dBTP when `true_peak` is on |
| `limit.release` | ms, log | 1 … 1000 | 50 | |
| `limit.lookahead` | ms | 0.5 … 10 | 1.5 | latency |
| `limit.true_peak` | bool | | true | |
| `limit.link` | percent | 0 … 100 | 100 | |
| `meter.gr` | dB, read-only | −60 … 0 | 0 | |

### 4.2 DSP

- Peak detection per channel on the input × `input` gain; with `true_peak`,
  on the 4× oversampled signal (`dsp::PolyphaseUpsampler`, the interpolator
  `core-analysis` uses for BS.1770 true peak): the detector value of a sample
  is the largest magnitude among its 4 interpolated points. The
  interpolator's delay (16 input samples) is absorbed by the lookahead
  window: the audio path is delayed by `lookahead + 16` samples, so the
  reported latency is `round(lookahead·fs) + 16` with true peak and
  `round(lookahead·fs)` without.
- Target gain `min(1, ceiling / peak)` per sample; the minimum over the
  lookahead window (a sliding-window minimum with a preallocated monotonic
  deque), then smoothed by a moving average over the same window (so the gain
  is fully down when the peak reaches the output), then released with a
  one-pole of `release`, never rising above the windowed target.
- Link: channels share `link·min(g_L, g_R) + (1 − link)·g_c`.
- A final sample clip at the ceiling catches rounding (no sample ever exceeds
  it in either mode); in the tests below it never removes more than 0.01 dB.

### 4.3 Behaviour tests

- ceiling holds: for sine bursts, clipped square waves, noise at +20 dBFS
  input and a 0.25·fs sine with inter-sample peaks, the output's true peak
  (measured with `core-analysis`) ≤ ceiling + 0.1 dB (true_peak on) and the
  sample peak ≤ ceiling (off);
- transparency below the ceiling: a −10 dBFS sine with ceiling −1 comes out
  delayed by the latency and otherwise within 1e-6;
- recovery: after a 10 ms +12 dB burst the gain returns to within 0.1 dB of
  unity after ≈ 5·release;
- latency reported equals the measured delay at 44.1/48/96 kHz.

### 4.4 GUI

Input/output peak meters with the ceiling line, a gain-reduction history,
large controls for input and ceiling, smaller ones for release, lookahead,
link and the true-peak toggle. Scenarios: `default`, `driven` (+9 dB input).

## 5. Trim — utility

### 5.1 Parameters

| key | type/unit | range | default | notes |
|---|---|---|---|---|
| `trim.gain` | dB | −60 … +24 | 0 | −60 = silence (`kOffAtMin`, displays −inf) |
| `trim.pan` | percent | −100 … +100 | 0 | balance: the opposite side falls, 0 dB at centre |
| `trim.width` | percent | 0 … 200 | 100 | side × width/100 |
| `trim.channels` | enum | `stereo`, `swap`, `left`, `right`, `mono`, `mid`, `side` | stereo | left/right: that channel on both outputs; mono: (L+R)/2 on both; mid/side: M or S on both |
| `trim.invert_left` | bool | | false | polarity |
| `trim.invert_right` | bool | | false | polarity |
| `trim.bass_mono` | Hz, log | 20 … 500 | 20 | side below this frequency removed (4th-order Linkwitz-Riley high-pass on S); 20 = off |
| `trim.dc_block` | bool | | false | 5 Hz one-pole high-pass |
| `trim.delay` | ms | 0 … 100 | 0 | intentional delay (not reported as latency) |
| `trim.mute` | bool | | false | |

Order: channels → polarity → dc block → width and bass mono (M/S) → pan →
gain → delay → mute (10 ms ramps for mute and channel changes).

### 5.2 Behaviour tests

- defaults are bit-exact; −60 dB is silence; +6 dB doubles;
- width 0 = mono sum (L = R = (L+R)/2 of the input); width 200 doubles S;
- pan +100: left silent, right unchanged; ±50 %: the documented law;
- bass mono 150 Hz: the side of a 50 Hz tone is below −40 dB, of a 1 kHz
  tone unchanged within 0.1 dB, and the mono sum of the output equals the
  input's mono sum (LR4 on S only keeps M untouched);
- delay 10 ms = 441/480 samples exactly at 44.1/48 kHz (rounded).

### 5.3 GUI

One compact panel (≤ 520 × 300): gain and pan knobs, width slider, channel
mode selector, polarity and mute toggles, bass-mono frequency, delay, a
small stereo correlation/level meter from telemetry. Scenario: `default`,
`wide` (width 150, bass mono 120 Hz).

## 6. Engine and benchmark use

Once built, the four plugins are in the build's `plugins/clap/`; the engine
finds them when its CLAP search path includes that directory. B1
proper uses Contour + Press per track and Margin on the master
(docs/benchmarks/); the MCP arrangement acceptance test gains a mixing pass
with them in a later round.
