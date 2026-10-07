# Spec: Orrery — the flagship synth

**Status:** Implemented; amendments are recorded at the end.
**Plugin:** `plugins/orrery`, CLAP id `com.suppleaudio.orrery`, instrument.
Built from the template per ADR-0011. An orrery is a clockwork model of
orbiting bodies: here, modulators orbiting a voice.

Orrery is the most important plugin in the project. Its patch format is its
primary interface: an LLM that has read this document once must be able to
write a working patch from a description, and the numbers in it must mean
what they say (Hz, ms, dB, st, ct, oct, %). A human gets a fast, legible GUI
over the same structure.

## 1. Design principles

1. **Constrained modularity.** A fixed pool of typed slots; the patch
   chooses which slots exist, their types, and how modulation flows. No
   free-form graphs, no cables to lose track of.
2. **Structure vs. values.** *Structure* — slot types, which slots exist,
   audio routing, the modulation route table, step values, FX order — lives
   only in the patch. *Values* — levels, frequencies, times, amounts of the
   existing slots, macros — are also CLAP parameters (automatable, ADR-0010
   normalized for generic hosts). Macros are the stable automation surface
   for anything structural.
3. **Modulation in musical units.** A route's amount is expressed in the
   destination's modulation unit: octaves for frequencies, semitones for
   pitch, dB for levels, % of range for everything else.
4. **The LLM chooses structure, the optimizer tunes numbers** (`optimizer.md`). The
   patch makes the continuous parameters of a topology easy to enumerate.
5. **Deterministic.** All randomness (drift, noise, S&H, random-per-note,
   unison phase) derives from the patch `seed`. Renders are block-size
   invariant (null to −120 dBFS across block sizes 1..4096).
6. **Real-time safe.** Structure changes build new voice DSP on the main
   thread and swap them in wait-free (semantic-params extension §5); the
   audio thread never allocates.

## 2. Signal flow (per voice, then global)

```
            ┌─ osc1 ─┐
 note ──────┼─ osc2 ─┼── mixer ──┬─ filter1 ─┬─(serial: filter1 → filter2 | parallel: sum)── drive ── amp ── pan ──┐
 (unison    ├─ osc3 ─┤   (per-osc │           │                                                                   │ voices
  copies)   └─ osc4 ─┘  level,pan,└─ filter2 ─┘                                                                   │ summed
                         target)                                                                                   ▼
                                                                     global: fx chain (chorus/phaser/delay/reverb/eq) ── output
 modulators per voice: amp env, env2..env4, lfo1..lfo4 (voice or global), step1..step2, velocity, key, random, MPE
 global: macro1..macro8, modwheel, pitchbend, aftertouch, expression, breath, free-running LFOs
```

Oscillators may also phase-modulate each other (FM) and amplitude-modulate
(AM/ring) through routes whose source is an oscillator (audio rate).

## 3. The patch document

Canonical member order as listed. Every block is optional except `schema`;
absent blocks take the defaults in §4. Absent slots do not exist (they
cost no CPU).

```json
{
  "schema": "supple.orrery-patch/1.0",
  "name": "Warm Evolving Pad",
  "tags": ["pad", "warm"],
  "notes": "Slow filter bloom; macro1 opens it up.",
  "seed": 1234,

  "voice": {
    "mode": "poly", "polyphony": 8,
    "unison": 3, "unison_detune": "12 ct", "unison_spread": "70 %",
    "glide": "0 ms", "glide_mode": "legato",
    "bend_range": "2 st", "drift": "10 %", "quality": "normal"
  },

  "oscillators": {
    "osc1": { "type": "va", "wave": "saw", "level": "-6 dB", "fine": "-7 ct", "to": "filter1" },
    "osc2": { "type": "va", "wave": "saw", "level": "-6 dB", "fine": "+7 ct", "to": "filter1" },
    "osc3": { "type": "sub", "wave": "sine", "octave": -1, "level": "-12 dB", "to": "filter1" }
  },

  "filters": {
    "routing": "serial",
    "filter1": { "type": "ladder", "slope": "24 dB", "cutoff": "1.2 kHz", "resonance": "25 %", "keytrack": "50 %" }
  },

  "drive": { "type": "tape", "amount": "3 dB", "mix": "100 %" },

  "amp": { "level": "-3 dB", "velocity": "40 %", "pan": "0 %" },

  "envelopes": {
    "amp":  { "attack": "600 ms", "decay": "1.2 s", "sustain": "80 %", "release": "1.5 s" },
    "env2": { "attack": "1.5 s", "decay": "3 s", "sustain": "30 %", "release": "2 s" }
  },

  "lfos": {
    "lfo1": { "wave": "sine", "rate": "0.2 Hz", "retrigger": "free" }
  },

  "steps": {},

  "routes": {
    "env2>filter1.cutoff":   { "amount": "+2 oct" },
    "lfo1>osc1.fine":        { "amount": "8 ct" },
    "macro1>filter1.cutoff": { "amount": "+3 oct" },
    "velocity>filter1.cutoff": { "amount": "+1 oct" }
  },

  "macros": { "macro1": { "name": "Brightness", "value": "20 %" } },

  "fx": {
    "chain": ["chorus", "reverb"],
    "chorus": { "rate": "0.4 Hz", "depth": "30 %", "mix": "35 %" },
    "reverb": { "size": "70 %", "decay": "3.5 s", "damping": "40 %", "mix": "25 %" }
  },

  "output": { "level": "0 dB" }
}
```

Numbers without units are in the canonical unit (Hz, ms, dB, st, ct, %).
The canonical form written by the plugin uses plain numbers (e.g.
`"cutoff": 1200.0`); strings with units are an input convenience.

## 4. Module reference

Ranges are physical; `curve` is the parameter's normalization (ADR-0010).
Defaults in **bold**. Keys of CLAP parameters are the patch path without
the block name: `osc1.level`, `filter1.cutoff`, `env2.attack`,
`fx.reverb.mix`, `macro1`.

### 4.1 `voice` (global)

| field | type | range / choices | default |
|---|---|---|---|
| `mode` | enum | `poly`, `mono`, `legato` | **poly** |
| `polyphony` | int | 1..32 | **8** |
| `unison` | int | 1..8 (voices per note) | **1** |
| `unison_detune` | ct | 0..100 (spread symmetric, max at the outer voices) | **10** |
| `unison_spread` | % | 0..100 (stereo spread of unison voices) | **50** |
| `glide` | ms | 0..5000, pow 3 | **0** |
| `glide_mode` | enum | `always`, `legato` | **legato** |
| `bend_range` | st | 0..48 | **2** |
| `mpe` | bool | per-note pitch/pressure/slide (bend range 48 st) | **false** |
| `drift` | % | 0..100: slow seeded per-voice pitch/cutoff drift (±5 ct / ±3 % at 100) | **0** |
| `quality` | enum | `eco` (1×), `normal` (2× oversampling for drive/filters/FM), `high` (4×) | **normal** |

### 4.2 `oscillators` — slots `osc1`..`osc4`

Common fields:

| field | type | range / choices | default |
|---|---|---|---|
| `type` | enum (structure) | `va`, `wavetable`, `fm`, `noise`, `sub` | — (required) |
| `level` | dB | −inf (off) .. +6, off-at-min at −60 | **−6** |
| `pan` | % | −100 (L) .. +100 (R), bipolar | **0** |
| `coarse` | st (int) | −48..+48 | **0** |
| `fine` | ct | −100..+100 | **0** |
| `phase_mode` | enum | `free` (free-running, seeded start) or `reset` (restart at `phase` on each note) | **free** |
| `phase` | deg | 0..360 (used when `phase_mode` is `reset`) | **0** |
| `to` | enum (structure) | `filter1`, `filter2`, `both`, `direct` (bypass filters) | **filter1** |

Per type:

- **`va`** — band-limited virtual analog (PolyBLEP/BLAMP or better; aliasing
  below −80 dB for fundamentals ≤ 4 kHz at 48 kHz in `normal`):
  `wave` `saw`/`square`/`pulse`/`triangle`/`sine` (**saw**);
  `width` % 1..99 pulse width for `pulse` (**50**);
  `shape` % 0..100 morph toward the next wave in the list (**0**);
  `sync` bool: hard-sync to osc1 (not on osc1) (**false**).
- **`wavetable`** — original, algorithmically generated tables (no third-party
  data), mip-mapped band-limited: `table` `harmonic_ramp` (saw → sine by
  harmonic roll-off), `pwm`, `formant` (vowel sweep), `bell` (inharmonic
  partial set rendered periodic), `organ` (drawbar sets), `digital` (bit/
  sample-rate reduction sweep), `spectral_comb` (**harmonic_ramp**);
  `position` % 0..100 (**0**).
- **`fm`** — sine operator with self-feedback, usable as carrier or
  modulator: `ratio` x 0.25..32, log, stepped to 0.25 when `ratio_mode` is
  `harmonic` (**1**); `ratio_mode` `harmonic`/`free`/`fixed` (**harmonic**);
  `fixed_freq` Hz 1..20000 (used when `fixed`) (**440**); `feedback` %
  0..100 (**0**); `wave` `sine`/`half_sine`/`abs_sine`/`square_sine`
  (**sine**). Other oscillators modulate it through routes to
  `oscN.fm` (§5.4).
- **`noise`** — `color` `white`/`pink`/`brown`/`blue` (**white**); `tilt`
  dB/oct −6..+6 on top of the color (**0**); seeded.
- **`sub`** — `wave` `sine`/`triangle`/`square` (**sine**); `octave` −1 or −2
  relative to the note (**−1**); no `coarse`/`fine` (it follows the note).

### 4.3 `filters` — `routing` + slots `filter1`, `filter2`

`routing`: `serial` (filter1 → filter2; oscillators targeting `filter2` enter
after filter1) or `parallel` (summed) (**serial**); `balance` % −100..100
between filter1/filter2 outputs in parallel (**0**).

Common: `type` (structure) `ladder`/`svf`/`comb`/`formant`; `cutoff` Hz
20..20000 log (**1000**); `resonance` % 0..100 (self-oscillation near 100
for ladder/svf) (**0**); `keytrack` % 0..200 (100 = cutoff follows pitch
1:1, reference C4) (**0**); `drive` dB 0..24 (input gain into the filter's
nonlinearity) (**0**); `level` dB −inf..+12 (**0**).

Per type:
- **`ladder`** — zero-delay-feedback 4-pole transistor-ladder model with
  per-stage saturation: `slope` `12 dB`/`24 dB` (**24 dB**).
- **`svf`** — ZDF state-variable (TPT) 2-pole: `mode`
  `lowpass`/`bandpass`/`highpass`/`notch`/`peak`/`allpass` (**lowpass**).
- **`comb`** — delay-line comb: `cutoff` sets the comb frequency; `feedback`
  % −99..+99 (negative = odd harmonics) (**50**); `damping` % 0..100 (**20**);
  `resonance` unused.
- **`formant`** — parallel resonator bank with vowel formant data (published
  phonetics averages): `vowel` 0..4 continuous a→e→i→o→u (**0**); `shift` st
  −12..+12 (**0**); `cutoff` unused.

### 4.4 `drive` (per voice, after filters)

`type` `soft`/`tape`/`fold`/`hard`/`off` (**off**) (antiderivative
anti-aliasing + oversampling per `quality`); `amount` dB 0..36 (**0**);
`bias` % −100..100 (asymmetry, even harmonics) (**0**); `mix` % 0..100
(**100**); output is gain-compensated.

### 4.5 `amp` (per voice)

`level` dB −inf..+12 (**0**); `velocity` % 0..100 (velocity → level
sensitivity; 100 = 40 dB range) (**50**); `pan` % −100..100 (**0**).
The amp envelope (`envelopes.amp`) always drives the amp.

### 4.6 `envelopes` — `amp`, `env2`..`env4`

`delay` ms 0..10000 pow 3 (**0**); `attack` ms 0..30000 pow 3 (**5**);
`hold` ms 0..10000 (**0**); `decay` ms 1..30000 pow 3 (**300**); `sustain` %
0..100 (**70**); `release` ms 1..30000 pow 3 (**300**); `attack_curve` %
−100..100 (0 linear, + exponential-like, − logarithmic) (**0**);
`decay_curve`, `release_curve` % −100..100 (**50**, i.e. exponential-ish);
`retrigger` `always`/`legato` (**always**). Output 0..1 (unipolar).

### 4.7 `lfos` — `lfo1`..`lfo4`

`wave` `sine`/`triangle`/`saw_up`/`saw_down`/`square`/`sample_hold`/`smooth_random`
(**sine**); `rate` Hz 0.01..50 log (**1**); `sync` enum `off` or note values
`8/1 4/1 2/1 1/1 1/2 1/2. 1/2t 1/4 1/4. 1/4t 1/8 1/8. 1/8t 1/16 1/16. 1/16t
1/32` (**off**; when not `off`, `rate` is ignored); `phase` deg 0..360
(**0**); `retrigger` `voice` (per voice, restarts at note on) / `free`
(global, free-running) / `mono` (global, restarts when all notes were
released) (**voice**); `fade` ms 0..10000 (**0**); `polarity` `bipolar`/
`unipolar` (**bipolar**). `free`/`mono` LFOs are global sources.

### 4.8 `steps` — `step1`, `step2`

`length` int 1..16 (**16**); `sync` note value (as LFO, no `off`) (**1/16**);
`values` array of 1..16 numbers in % −100..100 (structure; `length` entries
used, missing entries 0); `glide` % 0..100 (**0**); `retrigger` like LFOs
(**voice**). Output bipolar.

### 4.9 `macros` — `macro1`..`macro8`

`name` string ≤ 24 chars (structure, default "Macro N"); `value` % 0..100
(**0**). Macros are global CLAP parameters: automate these.

### 4.10 `fx` — global post-FX

`chain` array of up to 5 distinct FX names in processing order (structure);
blocks for the FX in the chain (others are ignored if present but not
written back). Each FX has `mix` % 0..100 unless noted.

- **`chorus`**: `rate` Hz 0.05..10 log (**0.5**), `depth` % (**30**),
  `voices` int 2..4 (**2**), `mix` (**50**).
- **`phaser`**: `rate` Hz 0.02..10 log (**0.3**), `depth` % (**50**),
  `stages` int 2..12 even (**6**), `feedback` % −90..90 (**30**), `mix` (**50**).
- **`delay`**: `sync` `off` or a note value as for LFOs (**1/8.**), `time` ms
  1..4000 log used when `sync` is `off` (**375**),
  `feedback` % 0..110 (safety-limited) (**35**), `ping_pong` bool (**false**),
  `damping` % (**30**), `mix` (**25**).
- **`reverb`**: `size` % (**50**), `decay` ms 100..30000 log (**2000**),
  `predelay` ms 0..250 (**10**), `damping` % (**40**), `width` % (**100**),
  `mix` (**20**).
- **`eq`**: `low_gain` dB −18..18 at 120 Hz shelf, `mid_freq` Hz 200..8000
  log, `mid_gain` dB −18..18, `mid_q` 0.3..8, `high_gain` dB −18..18 at
  8 kHz shelf (all **0** / 1 kHz / 0.7). No `mix`.

### 4.11 `output`

`level` dB −inf..+12 (**0**). A safety soft-clipper at 0 dBFS true-peak-ish
(+6 dB headroom knee) is always on after the FX; it never engages on a
patch whose output stays below −1 dBFS.

## 5. Modulation routes

### 5.1 Form

`routes` is an object keyed by `"<source>><destination>"` (the `>`
character, no spaces). Each value:

| field | type | meaning |
|---|---|---|
| `amount` | quantity | in the destination's modulation unit (§5.3); required |
| `via` | source name | optional scaling source: the amount is multiplied by `via` (0..1 for unipolar sources); e.g. `"via": "modwheel"` |
| `curve` | % −100..100 | optional response curve applied to the source (0 linear) |

At most 64 routes. The same source/destination pair appears once (the key
guarantees it); several sources may modulate one destination (they sum).

### 5.2 Sources

| source | rate | range | notes |
|---|---|---|---|
| `amp`, `env2`..`env4` | voice | 0..1 | |
| `lfo1`..`lfo4` | voice or global (per `retrigger`) | −1..1 or 0..1 | |
| `step1`, `step2` | voice or global | −1..1 | |
| `velocity` | voice | 0..1 | note-on velocity |
| `release_velocity` | voice | 0..1 | |
| `key` | voice | −1..1 | MIDI note, −1 at C−1, 0 at C4 (60), +1 at G9 (127); linear in semitones |
| `random` | voice | −1..1 | seeded, new per note |
| `gate` | voice | 0/1 | 1 while the key is held |
| `pressure` | voice | 0..1 | CLAP/poly/MPE pressure, else channel aftertouch |
| `slide` | voice | 0..1 | CLAP brightness or MPE timbre (CC74 per note), else CC74 |
| `modwheel`, `expression`, `breath` | global | 0..1 | CC1, CC11, CC2 |
| `pitchbend` | global | −1..1 | |
| `aftertouch` | global | 0..1 | channel pressure |
| `macro1`..`macro8` | global | 0..1 | |
| `osc1`..`osc4` | audio | −1..1 | only to `oscN.fm` / `oscN.am` destinations |

### 5.3 Destinations and units

Any CLAP parameter flagged modulatable (every continuous oscillator, filter,
drive, amp, envelope, LFO, step, FX and output field), plus:

- `oscN.pitch` (st; the whole oscillator pitch, both coarse and fine),
- `oscN.fm` (phase-modulation index, unit `x`: radians of peak phase
  deviation per unit source, 0..20) — source must be an oscillator or any
  control source for index modulation,
- `oscN.am` (%: 100 = full ring modulation) — source must be an oscillator.

Amount units by destination unit: Hz → **oct**; st, ct → **st**; dB → **dB**;
ms and everything else → **% of the parameter's range** (applied in the
normalized domain). Wrong units are validation errors that name the right
one (`"env2>filter1.cutoff": amount "500 Hz"` → "amount for filter1.cutoff
is in oct (octaves); e.g. \"+1.5 oct\"").

Global destinations (voice mode, FX, output, `free` LFO rates, macros) accept
only global sources; routing a voice source to them is an error that says
which sources are global.

### 5.4 Math

Modulation is summed per destination in its modulation domain (octaves,
semitones, dB, normalized %), added to the parameter's base value, and
clamped to the parameter's range. Audio-rate FM is phase modulation
(through-zero), with oversampling per `quality`.

## 6. Validation and errors

The patch is validated as a whole (core-patch + custom checks):

- slot types and type-specific fields: a field not valid for the slot's type
  is an error that lists the valid fields for that type;
- `to` referring to `filter2` when no `filter2` exists → error with fix;
- route keys: unknown sources/destinations (closest match), destinations of
  slots that don't exist ("osc3 is not in this patch — add it to
  `oscillators` or remove the route"), unit mismatches, voice→global
  violations, `osc*` sources to non-audio destinations;
- `fx.chain` entries unknown/duplicated;
- `steps.values` longer than 16;
- `sync` values not in the list.

Every error carries a JSON Pointer path and a concrete hint.

## 7. Describe

`describe(changed)` returns ≤ 600 characters, e.g.:

> Poly pad, 3-voice unison (12 ct). osc1+osc2 saw ±7 ct, osc3 sine sub −1 oct
> → ladder 24 dB LP 1.2 kHz res 25 %, keytrack 50 %. env2 (A 1.5 s D 3 s S 30 %)
> → cutoff +2 oct; lfo1 0.2 Hz → osc1 pitch 8 ct; velocity → cutoff +1 oct;
> macro1 "Brightness" → cutoff +3 oct. Amp A 600 ms D 1.2 s S 80 % R 1.5 s.
> FX: chorus 35 % → reverb 3.5 s 25 %.

## 8. CLAP parameters

All non-structure fields of all slots (existing or not) are CLAP parameters
with stable ids assigned in blocks (voice 0–99, osc1 100–199, osc2 200–299,
osc3 300–399, osc4 400–499, filters 500–699, drive 700, amp 720, envelopes
800–1199, lfos 1200–1599, steps 1600–1799 (not values), macros 1800–1807,
fx 2000–2499, output 2500). Parameters of absent slots are flagged hidden via
the semantic schema's `"active": false` and ignored by the DSP. Structure
fields are not parameters. Macros are the documented automation surface.

## 9. Performance targets (this machine)

8 voices × 3-voice unison, 2 va osc + sub, ladder 24 dB, drive, 2 envs,
1 LFO, chorus + reverb, `normal` quality: ≤ 3 % of one core at 48 kHz /
256 frames. Idle voices cost nothing (voice deactivation after release +
tail). Measured in `docs/benchmarks/`.

## 10. Evaluation

- **Descriptive targets** (each with a descriptor profile and a listening
  note): warm evolving pad; plucky bass with a short filter snap; metallic
  bell with inharmonic partials; supersaw lead; sub bass (clean sine
  fundamental ≤ 60 Hz, harmonics < −30 dB); FM electric piano; noise snare
  (with the sandbox's notation drum hit); vowel-y formant lead; acid bass
  (ladder res 80 %, env snap, glide); string ensemble.
- **Reference matching:** a hidden patch renders a phrase; the agent sees only
  the audio and its analysis, proposes a topology, runs the optimizer, and is
  scored by descriptor distance and LTAS RMS difference.
- The evaluation harness (`eval-harness.md`) runs these tasks; its renders
  are judged by listening.

## Amendments (2026-09-26, implementation of the core)

Recorded while building `plugins/orrery/engine` (params table, patch codec,
real-time engine). The code is the reference; these notes explain the choices.

- **Structure fields** (not CLAP parameters): slot existence and `type`
  (oscillators, filters, drive), `to`, `filters.routing`, `voice.quality`, LFO
  and step `retrigger` (it decides whether the modulator is a voice or a global
  source, which route validation depends on), `steps.*.values`, macro names,
  `fx.chain`, `routes`. Everything else is a parameter.
- **`to: "none"`** — an oscillator that is only heard through FM/AM routes
  (the usual FM modulator). The default target `filter1` passes straight
  through when no filter exists; `to` naming an absent `filter2` (or `both`
  without it) is an error. Absent filters are wires (serial: an oscillator sent to `filter2`
  without a `filter2` goes to the section output).
- **Oscillator `wave`** is one parameter with the union of choices; each type
  allows its subset (va: saw..sine; fm: sine, half_sine, abs_sine,
  square_sine; sub: sine, triangle, square) and has its own default (va saw,
  others sine). Fields a type does not have are errors listing the valid ones.
- **FM/AM routes** take only oscillator sources, and oscillator sources only
  go to `oscN.fm`/`oscN.am`. An oscillator's output (after its level) is the
  modulation signal, so the FM depth envelope is a route to the modulator's
  level (`env2>osc2.level`). The amount of `oscN.fm` is the peak phase
  deviation in radians per unit signal (`x`, ±20). Processing order is
  topological (modulators first); edges inside a cycle use the modulator's
  previous sample. `via` scales FM and AM depth per control tick; `curve` is
  an error on audio-rate routes (it shapes control sources only).
- **Route amounts** for `ct` destinations (e.g. `osc1.fine`) are in semitones
  like other pitch destinations; `st`, `ct` and `oct` strings convert into
  each other (`"8 ct"` → 0.08). Limits: ±10 oct, ±96 st, ±96 dB, ±20 x,
  ±100 %. Routes are written in canonical order (by source, then destination).
- **FX blocks** for FX not in `fx.chain` are errors (not silently ignored). A
  merge that removes an FX from the chain drops its block; a merge that changes
  a slot's `type` drops the old type's fields.
- **Choice labels in patches:** note values and the ladder slope are written
  with their labels (`"1/8."`, `"24 dB"`); the CLAP/core-params keys are
  `n1_8d`, `db24`. Both spellings parse.
- **Voice modes:** legato never restarts envelopes on overlapping keys; mono
  restarts envelopes whose `retrigger` is `always` (the default) — so the
  envelope `retrigger` field only matters in mono mode. Envelopes restart from
  their current level (no click).
- **Keyboard edits:** arrows step bool/int controls by one physical value and
  enums by one applicable choice, regardless of fine mode; floats use normalized
  steps of 0.01, or 0.002 in fine mode. Wave choices and Home/End bounds follow
  the current oscillator type, as in the choice menu.
- **Gain staging:** one voice with one oscillator at 0 dB peaks near
  −6 dBFS (fixed internal headroom); unison voices are normalized by
  1/√count. Oscillator and amp pan are constant power with unity at centre.
- **Control grid:** modulation is evaluated every 16 base-rate samples on a
  grid anchored at each voice's start (global modulators and FX parameters at
  absolute multiples of 16) and interpolated per sample; events apply at
  their sample time; parameter changes glide over the parameter's smoothing
  time from the event's sample. Renders are therefore bit-identical across
  host block sizes (tested for 1, 7, 16, 64, 333, 1024, 4096).
- **Oversampling:** voices are summed at the oversampled rate and decimated
  once by polyphase IIR half-band filters (−96 dB stopband from 28 kHz at 2×;
  4× cascades a 3-section first stage). No reported latency.
- **Resonance curves:** ladder feedback k = 4·r (self-oscillation at 100 %);
  SVF damping k = 2·(1 − r)², so the knob is perceptually even (40 % ≈ Q 1.4,
  70 % ≈ Q 5.6, 100 % rings). A linear damping mapping made resonance
  inaudible below ~60 % — the optimizer could not identify it.
- **Voices:** at most 64 sounding voices (notes × unison) plus 16 spares that
  fade stolen notes out over 64 samples; stealing prefers the oldest released
  note. When a burst of steals has used every spare, the new note takes over
  the fading voices closest to silence: a note-on never goes missing.
- **Note addressing:** note off, choke, poly pressure and supported CLAP
  note expressions match key, channel and note id conjunctively; each incoming
  `-1` component is a wildcard. All-wildcard off/choke targets every note.
  Mono/legato retain up to 128 held addresses (key, channel, id and velocity),
  each once, newest last; release restores the newest remaining address.
  Choke also removes matching held records.
- **Note expressions:** CLAP pressure, brightness and expression override the
  pressure, slide and expression sources for voice modulation, including an
  explicit zero. Without an override, the global controller source applies.
  CLAP volume, pan, tuning and vibrato expressions are unsupported. The wrapper
  ignores undersized event payloads and invalid note/MIDI addresses or data.
- **MIDI mode messages:** CC 120 (All Sound Off) fades every voice out at once
  and clears the pedal and held notes; CC 123 (All Notes Off) releases, and
  sustained notes wait for the pedal. A non-finite transport tempo counts as
  120 BPM.
- **Quality changes** keep sounding notes: their filters and feedback state
  restart at the new rate, envelopes, gate and pitch continue, and fades keep
  their duration.
- **`reset()`** clears every bit of processing state and keeps the parameter
  values. A render in a reused instance must match a fresh instance of the same
  patch, so `reset()` re-adopts the newest program on the next block (every
  ramp snaps to it, as after `prepare()`), the FX delay time starts at its value
  instead of gliding in from the previous render's target, and the control-tick
  bookkeeping and output-gain ramp restart (fix `4a9e8f5`). Guarded by
  `plugin.orrery.reset`. Re-prepare also clears voices, controllers, held notes
  and the performance clock while keeping the patch.
- **Validation:** `drive: {"type": "off"}` takes no other field, and an
  explicit empty step `values` array is an error.

- **Wavetables:** seven original tables generated at first use (FFT, main
  thread, ~8 MB shared by all instances) from harmonic recipes — `harmonic_ramp`
  (1/k^(1+8p²): saw → sine), `pwm` (pulse 50 % → 5 %), `formant` (a 1/√k source
  through three formants sweeping a → e → i → o → u for a nominal 110 Hz),
  `bell` (hum, prime, tierce, quint, nominal … with ratios quantized to 1/8 of
  the played pitch: the table's fundamental is pitch/8), `organ` (nine drawbars
  at pitch/2, eight registrations, 3 dB per step), `digital` (a ramp through
  sample-and-hold 16 → 6 steps and 5 → 3 bits, then band-limited; coarse from
  position 0 since 2026-09-30 so it never sounds like `harmonic_ramp`'s saw),
  `spectral_comb` (a raised-cosine comb over 1/k^0.6, period 16 → 3 harmonics).
  32 frames per table (`position` crossfades adjacent frames); 10 mip levels
  of 512 >> L harmonics at ≥ 8 samples per top harmonic, one gain per frame.
  The oscillator reads the level whose top harmonic stays below
  min(20 kHz, internal Nyquist): harmonics never reach the decimator's
  transition band. Measured: worst non-harmonic component −80 dB for a saw
  frame at 2.1 kHz in `normal`.
- **Plugin and editor:** `plugins/orrery` is the template-derived plugin
  (`Orrery.clap`; VST3 on request): the shared CLAP core (`plugins/orrery/clap`)
  plus an editor that shows the patch as a signal-flow diagram (oscillators →
  filters → drive → amp → fx → output), the modulators (envelopes, LFOs, steps,
  macros, voice), an inspector with the selected module's active parameters and
  the route list. Structural edits (add/remove/retype modules, targets, routing,
  quality, fx chain, routes and their `via`) are semantic merge patches built by
  `orrery/edits.h`, so the editor cannot produce a patch the codec rejects;
  parameter edits are host gestures. Outputs: stereo, or mono ((L+R)/2);
  32- and 64-bit hosts.

**Performance (2026-09-30, p3-orrery-perf):** the §9 reference patch now
measures 9.11 % of one core's CPU time versus 13.77 % for `508a229` in paired
Release runs pinned to CPU 0 (Core Ultra 5 225F, Clang 22.1.8, 48 kHz / 256,
median of three 30-second trials). Wall measurements were 10.66 % / 16.26 %;
the shared workstation's scheduling interference is substantial. The 3 %
target remains unmet. Portable four-wide float ladder lanes keep double phase
and coefficient ramps; classic oscillators and drive resolve their waveform /
curve once per segment, and unchanged control conversions are cached.

| Incremental step | Reference wall % |
|---|---:|
| Base, initial 30-second run | 14.79 |
| Classic oscillator specialization | 11.31 |
| Double ladder lanes (discarded experiment) | 12.18–12.76 |
| Float ladder lanes | 10.73 |
| Parameter starts / gain cache | 10.03 |
| Constant ramp fills | 10.82 |
| Drive specialization | 10.32 |
| Pitch/cutoff cache and muted drift evaluation | 9.46 |
| Final paired run | 10.66 (9.11 CPU) |

Intermediate runs show workstation variability, not isolated component costs.
Reference residual against the base is −137.84 dB relative to signal RMS;
every sound-design guide example passes −80 dB (worst −123.57 dB), with
bit-identical renders across block sizes. Remaining costs are structural:
per-voice oscillator recursions and staging (~43 % of sampled time), lane
filter gathers (~21 %), and repeated per-unison control evaluation (~16 %).
A persistent voice bank with double phase lanes, fused stages, and dependency-
checked shared note controls is the next pass. Full measurements, numerical
results, reproducible baseline capture, and verification commands are in
[the benchmark report](../benchmarks/orrery-perf-2026-09-30.md).

**Performance (orrery-bench, release, 48 kHz / 256):** the §9 reference patch
went from 18.0 % to 11.7 % of one core with bit-identical output: voices
render stage by stage per tick segment (type decisions hoisted out of the
sample loop), and the unison voices of a note render as a group whose filter
recursions run interleaved (the ladder is latency-bound per voice). What is
left: per-voice control ticks (~17 %), oscillators, drive. Next step toward 3 %:
float lanes across unison voices (SIMD), shared per-note control work.

**Performance (first measurement, 2026-09-26):** the §9 reference patch
(8 notes × 3 unison, 2 va + sub, ladder, tape drive, 2 envs, 1 LFO, chorus +
reverb, `normal`) costs ≈ 10.7 % of one core at 48 kHz / 256 frames (-O2),
against the 3 % target. Split: oscillators and mixing ≈ 5 %, ladder ≈ 3 %,
drive ≈ 2.6 %. Next: process voices in SIMD groups, hoist per-sample
invariants; tracked in `docs/benchmarks/`.
