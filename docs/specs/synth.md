# Spec: Orrery — the flagship synth

**Status:** Draft for review (2026-09-26); frozen before P3.2 starts.
**Plugin:** `plugins/orrery`, CLAP id `com.suppleaudio.orrery`, instrument.
Built from the template per ADR-0011. The name is a proposal (an orrery is a
clockwork model of orbiting bodies — modulators orbiting a voice); Nate may
veto it in `HUMAN_REVIEW.md`.

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
4. **The LLM chooses structure, the optimizer tunes numbers** (P3.3). The
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
| `pressure` | voice | 0..1 | poly/MPE pressure, else channel aftertouch |
| `slide` | voice | 0..1 | MPE timbre (CC74 per note), else CC74 |
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

## 10. Evaluation (P3.4)

- **Descriptive targets** (each with a descriptor profile and a listening
  note): warm evolving pad; plucky bass with a short filter snap; metallic
  bell with inharmonic partials; supersaw lead; sub bass (clean sine
  fundamental ≤ 60 Hz, harmonics < −30 dB); FM electric piano; noise snare
  (with the sandbox's notation drum hit); vowel-y formant lead; acid bass
  (ladder res 80 %, env snap, glide); string ensemble.
- **Reference matching:** a hidden patch renders a phrase; the agent sees only
  the audio and its analysis, proposes a topology, runs the optimizer, and is
  scored by descriptor distance and LTAS RMS difference.
- Success rates are tracked per plugin and skill revision in
  `docs/benchmarks/synth-eval.md`; listening checks go to `HUMAN_REVIEW.md`.
