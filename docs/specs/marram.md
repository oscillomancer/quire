# Spec: Marram — the stock synthesizer

**Status:** Implemented: the voice, its modulation and its editor. The
synth guide (`skill/guides/sound-design.md`) and the evaluation tasks use
it; Orrery, the hand-written synthesizer it replaced, was removed on
2026-10-21.
**Plugin:** `plugins/marram`, CLAP id `com.suppleaudio.marram`,
instrument. A DSP script compiled natively (ADR-0047),
`plugins/marram/script/marram.cpp`.

Marram rebuilds Orrery as a script. Orrery's patch is a structure (slots
that exist or not, a route table); a script has a fixed list of
parameters, so Marram gives every slot a type with an `off` and says which
parameters apply with the script's activity conditions (amendment of
2026-10-21 to `semantic-params-extension.md`): an agent's description and
the editor show only what the patch uses. Effects are not part of it:
delay, reverb, EQ and chorus are stock effects after it in a chain.

## 1. Signal flow (per voice)

```
 note ── osc1..osc4 ──(each: filter 1, filter 2, both, or direct)──┐
        (unison copies, FM between them, hard sync to osc1)         │
                                                                    ▼
          filter 1 ─┬─ serial: into filter 2 ─┬─ drive ── amp (env, velocity, pan) ── voices summed
          filter 2 ─┘  parallel: summed       ┘
 env2 (the mod envelope): cutoffs, pitch and FM depth, by fixed amounts
 routes (§3): amp, env2, env3, lfo1..3, macro1..4 and an instrument's own sources,
              to the numbers each voice reads
```

Every voice runs at the oversampled rate (`voice.quality`: eco 1x,
normal 2x, high 4x); the voices are summed and brought down once
(minimum-phase half-bands; latency 3 to 5 samples, reported). Modulation
is computed every 16 samples of a voice's life and glided between, so
renders do not depend on the block size; every random choice (free-running
phases, unison and drift, noise) is seeded by the order of the notes, so a
render is the same every time. Free-running LFOs count whole frames from
the last rate change, preserving phase without block-dependent rounding.
Free LFOs synced to a playing song also count whole frames from an anchor.
Host beat differences of at most **1e-9 beats** from the predicted position
are treated as rounding noise, so continuous playback is independent of
host block splits. Starting playback, enabling sync, changing division,
or a larger beat difference (a seek or loop jump) re-anchors to the song's
position. A tempo-only change preserves the current phase and advances
from it at the new tempo; a simultaneous seek or division change instead
takes the song's phase. Stopping playback invalidates the song anchor
and resumes the free-running clock; restarting takes the host position.
Sixteen voices (`voice.polyphony` plays
fewer); a stolen voice fades out over a few milliseconds. Its tail (CLAP's
`tail`, what a host waits for after the last key is up) is the longest amp
release, a voice's routes included, plus 20 ms. A safety limit leaves the output untouched below
−1 dBFS and bends it toward full scale above.

## 2. Parameters

Keys are the patch keys; units as in every semantic plugin. Defaults in
**bold**. "Applies" is the parameter's activity condition: an inactive
parameter keeps its value and is left out of descriptions.

### 2.1 `voice`

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `voice.mode` | `poly`, `mono`, `legato` | | **poly** | |
| `voice.polyphony` | voices at once; a note past them takes the oldest's | 1..16 | **16** | mode poly |
| `voice.glide` | ms | 0..5000 | **0** | |
| `voice.bend_range` | st | 0..48 | **2** | |
| `voice.tune` | ct, every oscillator; routes reach ±4800 | −100..100 | **0** | |
| `voice.unison` | copies of each pitched oscillator | 1..8 | **1** | |
| `voice.unison_detune` | ct, the outer copies each way | 0..100 | **12** | unison > 1 |
| `voice.unison_spread` | % across the stereo field | 0..100 | **60** | unison > 1 |
| `voice.drift` | % (100: pitch ±5 ct, each oscillator its own, cutoff ±3 %) | 0..100 | **0** | |
| `voice.quality` | `eco`, `normal`, `high` (restarts the plugin) | | **normal** | |

### 2.2 `osc1`..`osc4`

`oscN.type`: `off`, `analog`, `wavetable`, `fm`, `noise`, `sub` (**analog**
for osc1, **off** for the others).

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `oscN.level` | dB, off at the minimum | −60..6 | **−6** | not off |
| `oscN.pan` | % | −100..100 | **0** | not off |
| `oscN.coarse` | whole semitones | −48..48 | **0** | analog, wavetable, fm |
| `oscN.fine` | ct | −100..100 | **0** | analog, wavetable, fm |
| `oscN.wave` | `saw`, `square`, `pulse`, `triangle`, `sine` | | **saw** | analog, sub |
| `oscN.width` | % (the pulse's) | 1..99 | **25** | wave pulse |
| `oscN.sync` (osc2..4) | hard sync to osc1 | | **off** | analog |
| `oscN.table` | `harmonic_ramp`, `pwm`, `formant`, `bell`, `organ`, `digital`, `spectral_comb` | | **harmonic_ramp** | wavetable |
| `oscN.position` | % | 0..100 | **0** | wavetable |
| `oscN.ratio` | × the note's frequency, log | 0.25..16 | **1** | fm |
| `oscN.feedback` | % | 0..100 | **0** | fm |
| `oscN.color` | `white`, `pink`, `brown`, `blue` | | **white** | noise |
| `oscN.octave` | octaves under the note | −2..−1 | **−1** | sub |
| `oscN.fm` | the index: peak phase swing in radians | 0..10 | **0** | analog, wavetable, fm |
| `oscN.fm_from` | another oscillator, `oscM` | | the first other | fm > 0 |
| `oscN.to` | `filter1`, `filter2`, `both`, `direct` | | **filter1** | not off |
| `oscN.retrigger` | start each note at phase 0 (else free-running) | | **off** | neither off nor noise |

- **Analog** waves are polyBLEP/BLAMP band-limited (at C7, 2093 Hz, the
  energy away from the harmonics is −34 dB at eco, −55 dB at normal and
  −66 dB at high, as Orrery's).
- **Wavetables** are made when the plugin starts, from the same harmonic
  recipes as Orrery's (no third-party data): 16 frames a table, 9
  band-limited copies of each (256 harmonics at the top), the copy chosen
  for the pitch so that nothing passes 20 kHz. Each frame is as loud as a
  saw (RMS), its peak within 2, so sweeping the position holds the level.
  The bell's partials are in eighths of the note and the organ's on
  harmonics of half the note.
- **Harmonic ramp** and **digital** start at the analog saw itself: below
  their second frame (6.7 %) they play the analog saw's own samples,
  crossfading into the table, so at 0 % they null against it. The
  harmonic ramp then rolls the saw off to a sine (harmonic k at
  k^−(1 + 8p²)). Digital holds each frame and jumps to the next over the
  middle quarter of the way between them, through four kinds of four
  frames: the saw held on 8, 5 and 3 steps; a sine and two bent waves
  crushed to a few levels on 32 steps; sparse high harmonics (up to the
  31st) at 4 bits on 64 steps; and 1-bit patterns (three from sines,
  one a 15-step maximal-length sequence). Each frame's harmonics are those
  of its steps exactly, cut at the band level's top like any table's.
- **FM** is phase modulation by the source oscillator's waveform before
  its level: a modulator at the minimum level is silent and still
  modulates. Oscillators are computed from osc4 down, so a higher
  oscillator modulates a lower one in the same sample, a lower one a
  higher one a sample late. Unison copies modulate copy by copy.
- **Sub** follows the note an octave or two down, with the analog waves.
- **Noise** is made at the plugin's rate and held across the oversampled
  frame, so it sounds the same at every quality.
- **Unison** stacks up to 8 copies of each analog, wavetable or FM
  oscillator, detuned linearly to ± `unison_detune` and spread linearly
  across ± `unison_spread`, each at 1/√copies; sub and noise stay single.

### 2.3 `filter1`, `filter2`, `filters.routing`

`filterN.type`: `off`, `ladder`, `svf`, `comb`, `formant` (**ladder** for
filter 1, **off** for filter 2). `filters.routing`: `serial` (filter 1
into filter 2; oscillators sent to filter 2 join after filter 1) or
`parallel` (summed; `both` feeds both), applying while both filters are on.

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `filterN.cutoff` | Hz, log | 20..20000 | **2000** / **1000** | ladder, svf, comb (its pitch) |
| `filterN.resonance` | % | 0..100 | **10** | ladder, svf |
| `filterN.slope` | `db12`, `db24` | | **db24** | ladder |
| `filterN.mode` | `lowpass`, `bandpass`, `highpass`, `notch` | | **lowpass** | svf |
| `filterN.feedback` | % (negative: odd harmonics) | −99..99 | **50** | comb |
| `filterN.damping` | % | 0..100 | **20** | comb |
| `filterN.vowel` | a (0), e, i, o, u (4), blended | 0..4 | **0** | formant |
| `filterN.shift` | st | −12..12 | **0** | formant |
| `filterN.keytrack` | % from C4 (100: an octave per octave) | 0..200 | **0** | not off |
| `filterN.env` | oct at env2's peak | −8..8 | **2** / **0** | not off |
| `filterN.velocity` | oct at full velocity | 0..4 | **0** | not off |
| `filterN.drive` | dB into the filter | 0..24 | **0** | not off |

- **Ladder**: four zero-delay one-poles with their feedback solved at once,
  the input saturating (tanh); `resonance` 0..100 % is feedback 0..4, and
  it sings near 100 %. 12 dB taps the second stage.
- **SVF**: trapezoidal state-variable, damping 2(1 − r)² (40 % about Q 1.4,
  70 % about Q 5.6).
- **Comb**: a feedback comb tuned to `cutoff` (25 Hz at the lowest),
  `damping` a low-pass in the loop.
- **Formant**: three band-passes on averaged adult vowel formants,
  bandwidths 80/90/120 Hz widened to a Q of at most 6, keytrack moving
  them with the note.

### 2.4 `drive`, `amp`, `env2`, `env3`, `output`

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `drive.type` | `off`, `soft`, `tape`, `fold`, `hard` | | **off** | |
| `drive.amount` | dB, output compensated by 1/√gain | 0..36 | **6** | not off |
| `drive.bias` | % | −100..100 | **0** | not off |
| `drive.mix` | % | 0..100 | **100** | not off |
| `amp.attack` | ms | 0..30000 | **2** | |
| `amp.decay` | ms | 1..30000 | **600** | |
| `amp.sustain` | % | 0..100 | **80** | |
| `amp.release` | ms | 1..30000 | **250** | |
| `amp.velocity` | % (100: the softest note 40 dB down) | 0..100 | **50** | |
| `amp.level` | dB, each voice after the envelope | −48..12 | **0** | |
| `amp.pan` | % | −100..100 | **0** | |
| `env2.attack`, `env2.decay`, `env2.sustain`, `env2.release` | as the amp's | | **1**, **400**, **20**, **300** | |
| `env2.pitch` | st at the peak, every oscillator | −48..48 | **0** | |
| `env2.fm` | % of each FM depth that follows env2 | 0..100 | **0** | |
| `env3.attack`, `env3.decay`, `env3.sustain`, `env3.release` | as the amp's; a source for routes only | | **10**, **800**, **0**, **300** | |
| `output` | dB, smoothed | −36..12 | **0** | |

Envelope segments last exactly their time: the attack straight, decay and
release exponential in shape. An envelope restarts from where it is, so a
retrigger never clicks; Legato keeps both going between overlapping notes.
A note's level is env × velocity × the note's volume expression; its pan
adds the note's pan expression.

### 2.5 `lfo1`..`lfo3`, `macro1`..`macro4`

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `lfoN.shape` | `sine`, `triangle`, `saw_up`, `saw_down`, `square`, `random` (a new value each cycle), `drift` (gliding between them) | | **sine** | |
| `lfoN.sync` | follow the song's tempo | | **off** | |
| `lfoN.rate` | Hz, log | 0.01..50 | **2** | not synced |
| `lfoN.division` | `n1_32`, `n1_16`, `n1_8t`, `n1_8`, `n1_8d`, `n1_4t`, `n1_4`, `n1_4d`, `n1_2`, `bar1`, `bar2`, `bar4` | | **n1_4** | synced |
| `lfoN.mode` | `note` (each note's own, from its start), `free` (one for every voice, running on; synced to a playing song, from its position) | | **note** | |
| `lfoN.phase` | deg: where a note's starts, or how far a free one is moved | 0..360 | **0** | |
| `lfoN.delay` | ms: the fade-in from each note's start | 0..10000 | **0** | |
| `macroN` | %: a knob for routes (the source `macroN`) | 0..100 | **0** | |

An LFO's value is −1..1. Routes move a note LFO's rate per voice; a free
one runs at the parameter's own rate. Random and drift draw from a hash of
the cycle's number, so they are the same in every render.

## 3. Modulation

The patch's `routes` member (`semantic-params-extension.md`, amendment of
2026-10-21) connects sources to parameters, each with an amount in the
parameter's unit at the source's full value (a frequency's in octaves),
optionally through a second source:

```json
"routes": {
  "lfo1>voice.tune":     {"amount": "20 ct", "via": "modwheel"},
  "env3>osc2.fine":      {"amount": "+24 st"},
  "velocity>filter1.cutoff": {"amount": "+1 oct"},
  "macro1>drive.amount": {"amount": "12 dB"}
}
```

**Sources.** Per voice: `amp`, `env2`, `env3` (0..1), `lfo1`..`lfo3`
(−1..1), and an instrument's own `velocity`, `key`, `random`, `pressure` and
`slide`. For all voices: `macro1`..`macro4` (the knobs, 0..1) and
`modwheel`, `breath`, `expression`, `aftertouch`, `pitchbend`.

**Destinations** (every one per voice): `voice.tune`; each oscillator's
`level`, `pan`, `fine` (reaching ±4800 ct), `width`, `position`, `ratio`,
`feedback` and `fm`; each filter's `cutoff`, `resonance`, `feedback`,
`damping`, `vowel`, `shift` and `drive`; `drive.amount`, `drive.bias`,
`drive.mix`; `amp.level`, `amp.pan`, and the attack, decay and release of
the amp, Env 2 and Env 3; each LFO's `rate`. Choices, switches, unison,
glide and the output are not routed.

Every 16 samples of a voice (§1) the envelopes and LFOs move on, the
sources take their values, and the voice reads its destinations with their
routes, gliding to them; a destination's routes are kept within its range.
The fixed amounts of §2 (Env 2's, key tracking, velocity) still apply, and
add to the routes.

## 4. Factory patches

Dune Pad (a slow free LFO on the cutoff; the mod wheel opens it), Pluck
Bass, Acid Line, Supersaw (the mod wheel opens it), Sub Bass, Tine Piano
(FM; velocity deepens the bark, the mod wheel brings in an autopan), Glass
Bell (FM), Vowel Lead (formant; a delayed vibrato, a slow LFO between e
and i, the mod wheel changes the vowel), Strings (a delayed vibrato), Noise Snare, Drawbar Organ (wavetable,
a rotary shimmer), Wobble Bass (an LFO in eighths of the song), Sync Lead
(Env 3 sweeps a synced oscillator; vibrato under the mod wheel), Pulse Pad
(two LFOs on the pulse widths) and Table Sweep (Env 3 and a drifting LFO
through a wavetable; Macro 1 darkens it). Each measures −15.1 to −18.1 LUFS
on `dsp.test`'s notes signal (a note, then a chord, velocity 0.8).

## 5. The editor

The script's own, 1240 by 880 design units, drawn and edited in the
script (`editor()`, `draw()`, `pointer()`); the audio code sends the
output peaks, the voices sounding and the newest note's envelope and LFO
places as `Shared` members.

- **Header**: the name and a dune of grass, a blade a voice, lit while it
  sounds; the four macros; voices sounding of the limit; the output level
  and its meter.
- **Oscillators** (a tab each, with its type): its wave, two cycles drawn
  from the same code that makes it (a wavetable's frame at its position,
  an FM operator's ratio and depth, sync), and its controls. Controls that
  do not apply are dimmed, as the parameters' activity says.
- **Filters**: the two filters' combined response, computed from their
  parameters, with a node per filter to drag (cutoff across, resonance
  up); their routing; each filter's controls.
- **Envelopes** (amp, Env 2, Env 3): the shape with a handle per stage to
  drag, and where the newest note is in it.
- **LFOs**: the shape with its delay's fade and the newest note's phase.
- **Drive**: the type's transfer curve at the current drive.
- **Modulation**: the route list (`e.routes()`), the same as the automatic
  editor's for any script with routes.
- **Voice**: quality, mode, polyphony, glide, bend range, tuning, unison
  and drift.

Every dial drags (Shift: finely), turns by the wheel and resets on a
double click.

## 6. Performance

Release build, one core, 48 kHz: 8 notes of a pad with two saws and a sub,
3 unison copies, a 24 dB ladder and tape drive at normal quality, 4.1 %
with or without routes (Orrery's equivalent: 3.1 %).
