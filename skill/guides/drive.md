# Teasel: saturation and distortion

Use `com.suppleaudio.teasel` for colour and grit, from a trace of warmth on a
master to folding, crushing and ringing feedback. Patches use
`supple.teasel-patch/1.0`, with physical values under `params`; a merge
changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `style` | the curve (below) | tape |
| `drive` | dB, −12…48: gain into the curve | 6 |
| `character` | %, 0…100: each curve's own (below) | 30 |
| `bias` | %, −100…100: tilts the curve for even harmonics | 0 |
| `auto_gain` | bool: holds the level of a −18 dBFS RMS signal whatever the drive | true |
| `dynamics` | %, −100…100: + drives attacks harder, − the sustain and tails | 0 |
| `motion` | %, 0…100: the LFO's depth | 0 |
| `motion_rate` | Hz, 0.05…20 (when `motion_sync` is `free`) | 1 |
| `motion_sync` | `free`, `bars4`, `bars2`, `bar1`, `n2`, `n4`, `n8`, `n16`, `n32`, `n4t`, `n8t`, `n16t`, `n4d`, `n8d` | free |
| `motion_shape` | `sine`, `triangle`, `ramp_up`, `ramp_down`, `square`, `random`, `drift` | sine |
| `motion_target` | `drive` (±24 dB), `character` (±50), `bias` (±100), `pitch` (±2 octaves) at 100% | drive |
| `feedback` | %, −95…95: the curve's output back into it a period of the pitch later | 0 |
| `feedback_pitch` | Hz, 30…4000: the note the feedback rings at | 220 |
| `band_low` | Hz, 20…20000: below passes clean; 20 is off | 20 |
| `band_high` | Hz, 20…20000: above passes clean; 20000 is off | 20000 |
| `emphasis` | dB, −12…12: tilt about 1 kHz before the curve, undone after | 0 |
| `low_cut` | Hz, 20…2000 on what the curve gives; 20 is off | 20 |
| `high_cut` | Hz, 1000…20000 on what the curve gives; 20000 is off | 20000 |
| `mix` | %, 0…100: dry and driven | 100 |
| `output` | dB, −24…24 | 0 |
| `channels` | `stereo`, `mid` (centre only), `side` (width only) | stereo |
| `quality` | oversampling `x1`, `x2`, `x4`, `x8`, `x16` (at most 768 kHz inside); a restart | x4 |
| `phase` | the oversampling filters: `minimum` (3 to 5 samples of latency; the top octave's phase turns a little) or `linear` (no phase change; about 70 samples); a restart | minimum |
| `listen` | `off`, `band` (the driven band alone), `delta` (only what the drive changes) | off |
| `thd` | %, readonly: harmonics and noise (share of RMS) of a 100 Hz sine at −18 dBFS RMS through the curve as set | |
| `even` | %, readonly: the even harmonics' share of that sine's harmonic power | |
| `distortion` | %, readonly: the same measure on the music, over the last 300 ms | |
| `drive_now` | dB, readonly: the drive this moment, with dynamics and motion | |
| `input_level`, `output_level` | dBFS, readonly: peaks, falling 20 dB/s | |

## The curves

| Style | Family | Character | Sound |
|---|---|---|---|
| `soft` | warm | knee: 0 gentle from the start (third harmonic only), 100 clean longer, then turns | the most transparent; mastering |
| `tape` | warm | wear: a hysteresis loop near saturation | soft, round, odd harmonics |
| `tube` | warm | sag: loud peaks bias it colder for a while | second harmonic first; blocks when pushed |
| `transformer` | warm | core 40…400 Hz: below it the bass saturates first | thick low end, clean highs |
| `transistor` | bite | hardness: soft knee to near-hard | crunch, mostly odd |
| `diode` | bite | asymmetry: a symmetric pair to a single diode | grit from the first dB |
| `clip` | bite | knee: 0 hard clipping at the ceiling | clean until it clips; a clipper |
| `fuzz` | bite | starve: a dead zone that sputters on decays | high gain, squared off |
| `fold` | wild | edge: rounded (sine) to sharp (triangle) folds | metallic, rises then falls with drive |
| `rectify` | wild | full-wave share: half-wave to full-wave (octave up) | even harmonics, never subtle |
| `crush` | wild | bits: 16 down to 1 | quantised, gated quiet passages |
| `alias` | wild | rate: held at 20 kHz down to 250 Hz | sample-rate reduction, aliasing |

Bias adds an offset before the curve (removed after), so even harmonics
appear on any curve; on `transformer` it is a standing flux.

## Setting it by numbers

With `drive` 0 dB a −18 dBFS RMS signal sits well below every warm curve's
knee. Render with the plugin in place and read `thd` and `even`; they do not
depend on the music, so set them first:

| Aim | `thd` | Typical setting |
|---|---|---|
| Master, barely there | 0.1–0.5% | `soft` or `tape`, `drive` 0–4 |
| Bus colour | 0.5–2% | `tape` 4–8, or `tube` 4–6 with `bias` 10–20 |
| Obvious warmth, second harmonic | 1–4%, `even` above 60% | `tube`, `bias` 20–40 |
| Crunch | 5–15% | `transistor`, `diode` 12–18 |
| Heavy | 20%+ | `fuzz`, `clip` 24+ |

Then read `distortion` from a render of the real music: the share of what
the curve gives that is not a scaled copy of what went in. It follows the
music's level (a quiet passage barely touches the curve), so compare it
with `thd` to see how hard the music is hitting it.

## Recipes

Bus tape, transparent:

```json
{"params":{"style":"tape","drive":4,"character":25}}
```

Mid-only tube warmth on a master (the sides stay clean):

```json
{"params":{"style":"tube","drive":5,"bias":15,"character":20,"channels":"mid"}}
```

Bass grit that keeps the sub clean (drive only above 160 Hz):

```json
{"params":{"style":"tube","drive":18,"character":40,"bias":25,"band_low":160,"high_cut":7000}}
```

Drums: grit on the hits, tails clean, in parallel:

```json
{"params":{"style":"diode","drive":15,"character":50,"dynamics":50,"mix":50}}
```

Sustain thickened, attacks left clean:

```json
{"params":{"style":"fuzz","drive":18,"character":30,"dynamics":-70,"high_cut":6000}}
```

A clipper before a limiter (level rises with the drive), phase untouched:

```json
{"params":{"style":"clip","drive":3,"character":15,"auto_gain":false,"quality":"x8","phase":"linear"}}
```

A ringing, tuned distortion (pitch is the note's frequency):

```json
{"params":{"style":"transistor","drive":20,"feedback":70,"feedback_pitch":220,"high_cut":8000}}
```

Folds pulsing in eighth notes:

```json
{"params":{"style":"fold","drive":16,"motion":60,"motion_sync":"n8","motion_shape":"triangle","quality":"x8"}}
```

## Measuring it

`listen: "delta"` plays only what the drive changes; `"band"` the driven
band alone. With `auto_gain` on, a −18 dBFS RMS signal leaves as loud as it
came in at any drive (within about 0.5 dB on pink noise), so A/B renders
compare fairly; material far louder or quieter than that shifts by a few
dB, so compare loudness (`analyze`) before deciding more drive is better.
Check `thd` after changing style, drive, character or bias.

## Limits

`transformer` holds the mids' level; bass that saturates loses level, as an
iron core does (about 4 dB on pink noise at 24 dB of drive). `fold` and
`clip` at high drive need `x8` or `x16`: at 4x a 5 kHz sine driven 24 dB
into `clip` leaves aliases about 45 dB down, at 16x about 70. `rectify`
and `alias` are never subtle. Feedback never sustains itself, and lets go
a second after the input falls digitally silent; at high drive with
`rectify` and strong feedback peaks can pass 0 dBFS: trim `output`.
Changing `quality` or `phase` restarts the plugin. On a master, or
anywhere the processed signal is compared or blended with the original,
use `phase: "linear"`; elsewhere `minimum` keeps the latency down.

See [mixing](mixing.md) and
[stock spec §5](../../docs/specs/stock-effects.md#5-saturation--teasel-teasel).
