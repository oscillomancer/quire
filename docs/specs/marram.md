# Spec: Marram — the stock synthesizer

**Status:** In progress. The voice (this document) is implemented;
modulation routes, the editor, the agent guide and the evaluation tasks
follow, and Orrery (`synth.md`) stays the documented synthesizer until
they are done.
**Plugin:** `plugins/stock/marram`, CLAP id `com.suppleaudio.marram`,
instrument. A DSP script compiled natively (ADR-0047),
`plugins/stock/marram/script/marram.cpp`.

Marram rebuilds Orrery as a script. Orrery's patch is a structure (slots
that exist or not, a route table); a script has a fixed list of
parameters, so Marram gives every slot a type with an `off` and says which
parameters apply with the script's activity conditions (amendment of
2026-10-21 to `semantic-params-extension.md`): an agent's description and
the editor show only what the patch uses. Effects are not part of it:
chorus, delay, reverb and EQ are the stock effects after it in a chain.

## 1. Signal flow (per voice)

```
 note ── osc1..osc4 ──(each: filter 1, filter 2, both, or direct)──┐
        (unison copies, FM between them, hard sync to osc1)         │
                                                                    ▼
          filter 1 ─┬─ serial: into filter 2 ─┬─ drive ── amp (env, velocity, pan) ── voices summed
          filter 2 ─┘  parallel: summed       ┘
 env2 (the mod envelope): cutoffs, pitch and FM depth, by fixed amounts
```

Every voice runs at the oversampled rate (`voice.quality`: eco 1x,
normal 2x, high 4x); the voices are summed and brought down once
(minimum-phase half-bands; latency 3 to 5 samples, reported). Modulation
is computed every 16 samples of a voice's life and glided between, so
renders do not depend on the block size; every random choice (free-running
phases, unison and drift, noise) is seeded by the order of the notes, so a
render is the same every time. Sixteen voices; a stolen voice fades out
over a few milliseconds. A safety limit leaves the output untouched below
−1 dBFS and bends it toward full scale above.

## 2. Parameters

Keys are the patch keys; units as in every semantic plugin. Defaults in
**bold**. "Applies" is the parameter's activity condition: an inactive
parameter keeps its value and is left out of descriptions.

### 2.1 `voice`

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `voice.mode` | `poly`, `mono`, `legato` | | **poly** | |
| `voice.glide` | ms | 0..5000 | **0** | |
| `voice.bend_range` | st | 0..48 | **2** | |
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

### 2.4 `drive`, `amp`, `env2`, `output`

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
| `amp.pan` | % | −100..100 | **0** | |
| `env2.attack`, `env2.decay`, `env2.sustain`, `env2.release` | as the amp's | | **1**, **400**, **20**, **300** | |
| `env2.pitch` | st at the peak, every oscillator | −48..48 | **0** | |
| `env2.fm` | % of each FM depth that follows env2 | 0..100 | **0** | |
| `output` | dB, smoothed | −36..12 | **0** | |

Envelope segments last exactly their time: the attack straight, decay and
release exponential in shape. An envelope restarts from where it is, so a
retrigger never clicks; Legato keeps both going between overlapping notes.
A note's level is env × velocity × the note's volume expression; its pan
adds the note's pan expression.

## 3. Factory patches

Dune Pad, Pluck Bass, Acid Line, Supersaw, Sub Bass, Tine Piano (FM),
Glass Bell (FM), Vowel Lead (formant), Strings, Noise Snare, Drawbar Organ
(wavetable). Each measures −15.8 to −18.1 LUFS on `dsp.test`'s notes
signal (a note, then a chord, velocity 0.8).

## 4. Not yet (each a later step)

- **Modulation**: LFOs, more envelopes, macros, the mod wheel, aftertouch
  and the other sources, and a route table with amounts in the
  destination's unit (a framework feature, so that every script can use
  it). Until then the fixed amounts above are the modulation.
- **Polyphony** below 16 voices (the runtime's voice count is fixed).
- **Its own editor**: the automatic one shows every group and dims what
  does not apply.
- **The agent guide and evaluation tasks**, then Orrery's removal.

## 5. Performance

Release build, one core, 48 kHz: 8 notes of a pad with two saws and a sub,
3 unison copies, a 24 dB ladder and tape drive at normal quality, 3.9 %
(Orrery's equivalent: 3.1 %).
