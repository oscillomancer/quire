# Designing sounds with Orrery

Orrery is Quire's synthesizer. You program it with a patch: a JSON document
that says which modules exist, how they connect, and what their settings are.
The exact fields, units and ranges are in `generated/plugins/orrery.md`; this
guide is about *how to get to a sound*.

## The division of labour

You choose **structure**: which oscillators (and their types), which filters,
whether there is drive, which envelopes/LFOs exist, and which **routes**
connect sources to destinations. The **optimizer** tunes the continuous
numbers against a target. You are good at the first and bad at nudging twenty
coupled knobs by ear; the optimizer is the opposite. So: pick a topology from
the recipes below, set a sensible first guess, measure, and hand the numbers
to `optimize_start`.

## Anatomy in one screen

```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"polyphony": 8, "unison": 3, "unison_detune": "12 ct", "quality": "normal"},
 "oscillators": {
   "osc1": {"type": "va", "wave": "saw", "to": "filter1"},
   "osc2": {"type": "sub", "wave": "sine", "octave": -1, "level": "-9 dB", "to": "filter1"}},
 "filters": {"filter1": {"type": "ladder", "slope": "24 dB", "cutoff": "900 Hz", "resonance": "30 %"}},
 "envelopes": {"amp": {"attack": "5 ms", "decay": "400 ms", "sustain": "60 %", "release": "300 ms"},
               "env2": {"attack": "1 ms", "decay": "250 ms", "sustain": 0}},
 "routes": {"env2>filter1.cutoff": {"amount": "+2.5 oct"}}}
```

- **Oscillators** `osc1`..`osc4`: `va` (saw/square/pulse/triangle/sine with
  `width`, `shape`, `sync`), `wavetable` (`table`, `position`), `fm` (sine
  operator: `ratio`, `feedback`), `noise` (`color`, `tilt`), `sub` (`octave`).
  `to` sends it to `filter1`, `filter2`, `both`, `direct` (no filter) or
  `none` (heard only through FM/AM routes).
- **Filters** `filter1`, `filter2`: `ladder` (warm 12/24 dB low-pass),
  `svf` (low/band/high-pass, notch, peak), `comb`, `formant` (vowels).
  `filters.routing` is `serial` or `parallel`.
- **Drive** (`soft`, `tape`, `fold`, `hard`) after the filters, then the amp.
- **Envelopes**: `amp` always exists; `env2`..`env4` are yours to route.
  **LFOs** `lfo1`..`lfo4`, **steps** `step1`, `step2` (up to 16 values),
  **macros** `macro1`..`macro8` (the automation surface — name them).
- **Routes**: `"<source>><destination>": {"amount": …}`. The amount is in the
  destination's unit: `oct` for frequencies, `st` for pitch, `dB` for levels,
  `x` for FM depth (`oscN.fm`), `%` otherwise. `via` scales a route by a second
  source (`"via": "modwheel"`).
- **FX**: `"fx": {"chain": ["chorus", "reverb"]}` then each effect's block.

Omitted fields take their defaults. A patch only needs what you care about.

## The workflow

1. `plugin_load` Orrery, then `patch_set` your topology (from a recipe).
2. `render` a phrase that exercises the sound (a held chord for pads, repeated
   short notes for basses and plucks) with `"analyze": "summary"`.
3. Change **one idea** at a time with `patch_merge`, render, and `compare`
   with the previous render.
4. When the structure is right, run `optimize_start` with the 3–8 parameters
   that matter (flat keys like `filter1.cutoff`, `env2.decay`) and a target:
   a reference render/file, or a profile of descriptor ranges. `job_wait`,
   then `patch_merge` the result's `patch`.
5. `patch_get {"detail": "changed"}` is the short form to report; `describe`
   gives a one-line summary for the human.

## Which knob moves which number

| You want | Change | Watch |
|---|---|---|
| brighter / darker | `filter1.cutoff`, env→cutoff amount, osc `wave`/`table` | `spectral.centroid_hz`, band levels |
| more bite on attack | env2→cutoff amount and `env2.decay` | centroid trajectory, `envelope.attack_ms` |
| softer start | `amp.attack` | `envelope.attack_ms` |
| shorter / longer tail | `amp.decay`/`sustain`/`release`, reverb `decay` | `envelope.decay20_ms`, release |
| wider | `voice.unison` + `unison_spread`, chorus | `stereo.width` |
| thicker | unison + `unison_detune`, a detuned second osc | band levels, flatness |
| more weight | a `sub` oscillator, `drive` | `sub`/`low` bands |
| movement | LFO → cutoff/position/pitch, steps | `spectral.flux` |
| inharmonic / metallic | `fm` with a non-integer `ratio` (`"ratio_mode": "free"`), `wavetable` `bell`, `comb` filter | `harmonic.inharmonicity` |
| vowel-like | `formant` filter or the `formant` table; route an LFO to `vowel`/`position` | band peaks |

## Pitfalls

- **Scope:** per-voice sources (envelopes, velocity, key, voice LFOs) cannot
  modulate global parameters (fx, output, free-running LFO rates). The error
  names the rule; use a macro or a free LFO instead.
- **Flat params need their module:** `{"params": {"filter2.cutoff": 800}}`
  fails if `filter2` doesn't exist — add the slot first.
- **Changing a type drops fields** the new type doesn't have (and routes to
  them). Re-read the patch after a type change.
- **Loudness:** don't win by getting louder; the optimizer excludes
  `output.level` for this reason. Compare at matched loudness.
- **Resonance** is perceptual: 40 % is a gentle bump, 70 % clearly resonant,
  ≥ 90 % rings (the ladder self-oscillates near 100 %).
- **Quality:** `normal` (2× oversampling) is right for almost everything;
  `high` for heavy FM/sync/drive, `eco` when CPU matters more than aliasing.

## Recipes (starting points, not answers)

Each is a complete patch for `patch_set`; the line after it names the
parameters worth optimizing.

**Warm pad**
```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"polyphony": 8, "unison": 3, "unison_detune": "12 ct", "unison_spread": "70 %"},
 "oscillators": {"osc1": {"type": "va", "wave": "saw", "level": "-6 dB", "fine": "-7 ct"},
                 "osc2": {"type": "va", "wave": "saw", "level": "-6 dB", "fine": "+7 ct"}},
 "filters": {"filter1": {"type": "ladder", "slope": "24 dB", "cutoff": "1.2 kHz", "resonance": "25 %"}},
 "envelopes": {"amp": {"attack": "600 ms", "decay": "1.2 s", "sustain": "80 %", "release": "1.5 s"},
               "env2": {"attack": "1.5 s", "decay": "3 s", "sustain": "30 %", "release": "2 s"}},
 "lfos": {"lfo1": {"rate": "0.2 Hz", "retrigger": "free"}},
 "routes": {"env2>filter1.cutoff": {"amount": "+2 oct"}, "lfo1>osc1.fine": {"amount": "0.08 st"}},
 "fx": {"chain": ["chorus", "reverb"], "reverb": {"size": "70 %", "mix": "25 %"}}}
```
Optimize: `filter1.cutoff`, `env2.attack`, `env2.decay`, `amp.attack`.

**Plucky bass with a filter snap**
```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"mode": "mono"},
 "oscillators": {"osc1": {"type": "va", "wave": "saw"},
                 "osc2": {"type": "sub", "wave": "sine", "octave": -1, "level": "-6 dB"}},
 "filters": {"filter1": {"type": "ladder", "slope": "24 dB", "cutoff": "300 Hz", "resonance": "35 %"}},
 "envelopes": {"amp": {"attack": "2 ms", "decay": "400 ms", "sustain": "40 %", "release": "80 ms"},
               "env2": {"attack": "0 ms", "decay": "180 ms", "sustain": 0}},
 "routes": {"env2>filter1.cutoff": {"amount": "+3 oct"}, "velocity>filter1.cutoff": {"amount": "+1 oct"}}}
```
Optimize: `filter1.cutoff`, `filter1.resonance`, `env2.decay`, `amp.decay`.

**Acid bass** (mono, glide, resonant snap)
```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"mode": "legato", "glide": "60 ms"},
 "oscillators": {"osc1": {"type": "va", "wave": "saw"}},
 "filters": {"filter1": {"type": "ladder", "slope": "24 dB", "cutoff": "400 Hz", "resonance": "80 %"}},
 "drive": {"type": "soft", "amount": "6 dB"},
 "envelopes": {"amp": {"attack": "1 ms", "sustain": "80 %", "release": "60 ms"},
               "env2": {"attack": "0 ms", "decay": "200 ms", "sustain": 0}},
 "routes": {"env2>filter1.cutoff": {"amount": "+3 oct"}}}
```
Optimize: `filter1.cutoff`, `env2.decay`, `drive.amount`.

**Sub bass** (clean fundamental)
```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"mode": "mono", "glide": "20 ms"},
 "oscillators": {"osc1": {"type": "va", "wave": "sine", "to": "direct"}},
 "envelopes": {"amp": {"attack": "3 ms", "sustain": "100 %", "release": "80 ms"}}}
```
Optimize: usually nothing; add `"drive": {"type": "soft"}` for audible harmonics.

**Supersaw lead**
```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"unison": 7, "unison_detune": "25 ct", "unison_spread": "80 %"},
 "oscillators": {"osc1": {"type": "va", "wave": "saw"}},
 "filters": {"filter1": {"type": "svf", "mode": "lowpass", "cutoff": "6 kHz", "resonance": "10 %"}},
 "envelopes": {"amp": {"attack": "5 ms", "sustain": "90 %", "release": "250 ms"}},
 "fx": {"chain": ["delay", "reverb"], "delay": {"mix": "20 %"}, "reverb": {"mix": "15 %"}}}
```
Optimize: `voice.unison_detune`, `filter1.cutoff`.

**Metallic bell** (inharmonic partials)
```json
{"schema": "supple.orrery-patch/1.0",
 "oscillators": {"osc1": {"type": "fm", "ratio": 1},
                 "osc2": {"type": "fm", "ratio": 3.5, "ratio_mode": "free", "level": "-12 dB", "to": "none"}},
 "envelopes": {"amp": {"attack": "0 ms", "decay": "3 s", "sustain": 0, "release": "2 s"},
               "env2": {"attack": "0 ms", "decay": "1.5 s", "sustain": 0}},
 "routes": {"osc2>osc1.fm": {"amount": 3}, "env2>osc2.level": {"amount": "+12 dB"}},
 "fx": {"chain": ["reverb"]}}
```
Or `{"type": "wavetable", "table": "bell"}` for a ready-made partial set.
Optimize: `osc2.ratio`, `osc2.level`, `env2.decay`, `amp.decay`.

**FM electric piano**
```json
{"schema": "supple.orrery-patch/1.0",
 "oscillators": {"osc1": {"type": "fm", "ratio": 1},
                 "osc2": {"type": "fm", "ratio": 14, "level": "-30 dB", "to": "none"}},
 "envelopes": {"amp": {"attack": "1 ms", "decay": "1.5 s", "sustain": "20 %", "release": "300 ms"},
               "env2": {"attack": "0 ms", "decay": "400 ms", "sustain": 0}},
 "routes": {"osc2>osc1.fm": {"amount": 1.5}, "env2>osc2.level": {"amount": "+24 dB"},
            "velocity>osc2.level": {"amount": "+6 dB"}},
 "fx": {"chain": ["chorus"]}}
```
Optimize: `osc2.level`, `env2.decay`, `amp.decay`.

**Noise snare** (with a drum hit in notation)
```json
{"schema": "supple.orrery-patch/1.0",
 "oscillators": {"osc1": {"type": "noise", "color": "white"},
                 "osc2": {"type": "va", "wave": "triangle", "coarse": -12, "level": "-6 dB", "to": "direct"}},
 "filters": {"filter1": {"type": "svf", "mode": "bandpass", "cutoff": "1.8 kHz", "resonance": "20 %"}},
 "envelopes": {"amp": {"attack": "0 ms", "decay": "150 ms", "sustain": 0, "release": "100 ms"},
               "env2": {"attack": "0 ms", "decay": "40 ms", "sustain": 0}},
 "routes": {"env2>osc2.pitch": {"amount": "+12 st"}}}
```
Optimize: `filter1.cutoff`, `amp.decay`, `osc2.level`.

**Vowel lead** (formant sweep)
```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"mode": "legato", "glide": "40 ms"},
 "oscillators": {"osc1": {"type": "wavetable", "table": "formant", "position": "30 %"}},
 "lfos": {"lfo1": {"rate": "0.5 Hz"}},
 "envelopes": {"amp": {"attack": "20 ms", "sustain": "90 %", "release": "200 ms"}},
 "routes": {"lfo1>osc1.position": {"amount": "40 %"}},
 "fx": {"chain": ["delay"]}}
```
Optimize: `osc1.position`, `lfo1.rate`.

**String ensemble**
```json
{"schema": "supple.orrery-patch/1.0",
 "voice": {"unison": 3, "unison_detune": "10 ct", "unison_spread": "90 %"},
 "oscillators": {"osc1": {"type": "va", "wave": "saw"},
                 "osc2": {"type": "va", "wave": "saw", "fine": "+8 ct", "level": "-3 dB"}},
 "filters": {"filter1": {"type": "svf", "mode": "lowpass", "cutoff": "3 kHz", "resonance": "5 %"}},
 "envelopes": {"amp": {"attack": "400 ms", "sustain": "90 %", "release": "800 ms"}},
 "lfos": {"lfo1": {"rate": "5 Hz", "fade": "600 ms"}},
 "routes": {"lfo1>osc1.fine": {"amount": "0.05 st"}},
 "fx": {"chain": ["chorus", "reverb"], "reverb": {"size": "80 %", "mix": "30 %"}}}
```
Optimize: `filter1.cutoff`, `amp.attack`, `voice.unison_detune`.
