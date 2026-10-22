# Pollia: phasing

Use `com.suppleaudio.pollia` for phasing: a slow swirl on a pad or a
guitar, a resonant sweep, the throb of a photocell vibe, notches that
follow the playing (an envelope phaser), sweeps locked to the song, and
the barber pole, notches that rise or fall forever. Patches use
`supple.pollia-patch/1.0`, with physical values under `params`; a merge
changes only the keys it names.

A phaser mixes the sound with a copy passed through a chain of all-pass
stages. Each frequency comes out of the chain delayed by its own amount;
where it comes out half a cycle late it cancels the dry sound: a notch.
Every two stages make one notch, and moving the stages moves the notches.

| Key | Unit and range | Default |
|---|---|---|
| `stages` | integer, 1…24: every two make a notch (4: two notches, 12: six) | 6 |
| `centre` | Hz, 30…10000: where the stages sit at the middle of the sweep | 700 |
| `spread` | oct, 0…6: the stages staggered over this many octaves; 0 stacks them (even, deep notches) | 0 |
| `feedback` | %, −95…95: positive rings at the peaks between notches, negative moves the peaks | 0 |
| `motion` | `sweep` (back and forth), `rise`, `fall` (a barber pole: notches climb or sink forever) | sweep |
| `range` | oct, 0…8: how far a sweep moves the stages, top to bottom (sweep only) | 3 |
| `sync` | `free`, `bars8`, `bars4`, `bars2`, `bar1`, `n2`, `n4`, `n8`, `n16`, `n2d`, `n4d`, `n8d`, `n2t`, `n4t`, `n8t` (`d` dotted, `t` triplet) | free |
| `rate` | Hz, 0.01…20: cycles a second when `sync` is `free` | 0.3 |
| `shape` | `sine`, `triangle`, `ramp_up`, `ramp_down`, `steps` (a new place each cycle), `drift` (wandering) | sine |
| `phase` | degrees, 0…360: moves the motion along its cycle; synced, where it is at each bar | 0 |
| `stereo` | degrees, 0…180: the right side's motion ahead of the left's | 0 |
| `response` | `direct`, or `photocell`: a lamp the cell follows, quick up and slow down | direct |
| `envelope` | oct, −4…4: how far the playing moves the stages (up or down) at its loudest | 0 |
| `attack` | ms, 1…500: how fast the envelope follows a louder sound | 10 |
| `release` | ms, 10…3000: how fast it falls back | 200 |
| `keep_below` | Hz, 20…1000: frequencies below pass untouched; 20 is off | 20 |
| `mix` | %, 0…100: 50 the deepest notches, 100 the wet alone (a vibrato) | 50 |
| `output` | dB, −24…24 | 0 |
| `centre_now` | Hz, readonly: where the left side's stages sit this moment | |
| `envelope_now` | %, readonly: the envelope, 100 at the playing's recent peak | |
| `input_level`, `output_level` | dBFS, readonly: peaks, falling 20 dB/s | |

## Where the notches are

With `spread` 0 and `feedback` 0, n stages at a centre f put notches at
f × tan(90° × (2k + 1) / n) for k = 0, 1, …: four stages at 1 kHz notch at
414 Hz and 2.4 kHz, two stages notch at the centre itself. A sweep moves
the centre `range` octaves, half above and half below `centre`: range 3
at 700 Hz sweeps the centre from 250 Hz to 2 kHz. `spread` widens the
chain's span, so the notches spread unevenly over more octaves (a vibe's
sound). Feedback narrows the peaks and fills the notches a little: at 50%
a notch is about 16 dB deep instead of complete.

`motion` `rise` and `fall` keep the stages still and turn the wet sound's
phase against the dry's, one turn per cycle: each notch slides to where
the next one was, so the notches climb (or sink) without end and stay
full depth. `rate` (or `sync`) is how often a notch passes a place; `spread`
of 3 to 5 octaves spaces the notches evenly across the band, which is the
classic barber pole. With `stereo` 180 the right side's notches sit
between the left's.

## Motion

| Want | Setting |
|---|---|
| Slow swirl | `rate` 0.1–0.4, `range` 3, `shape` `sine` |
| Even, classic sweep | `shape` `triangle` (the notches move at an even pace in octaves) |
| A sweep per bar or two | `sync` `bar1` or `bars2`; `phase` puts the top or bottom on the downbeat |
| Rhythmic jumps | `shape` `steps`, `sync` `n8` or `n16` |
| Wide | `stereo` 90 (a swirl between the speakers) or 180 |
| Lopsided throb | `response` `photocell`, `rate` 3–7 Hz, `stages` 4, `spread` 3 |
| Pitch wobble (vibrato) | `mix` 100; `range` and `rate` set its depth and speed |
| Auto-wah-like | `range` 0, `envelope` +3, short `attack`, `feedback` 50 |
| No motion (a fixed notch comb) | `range` 0: then automate `centre` |

The envelope measures the playing against its own recent peak (falling
4 dB a second), so it needs no threshold: a note as loud as the last loud
one moves the stages the full `envelope`, one 24 dB quieter not at all.
It adds to the sweep and works in `rise` and `fall` too.

## Recipes

A classic four-stage swirl:

```json
{"params":{"stages":4,"centre":600,"range":3.5,"rate":0.5,"shape":"triangle"}}
```

A resonant sweep on a pad or synth:

```json
{"params":{"stages":8,"centre":900,"range":3,"feedback":70,"rate":0.2}}
```

The photocell vibe's throb (and with `mix` 100, its vibrato):

```json
{"params":{"stages":4,"spread":3,"centre":900,"range":3,"rate":4.5,"feedback":15,"response":"photocell"}}
```

An endless rise (a barber pole):

```json
{"params":{"motion":"rise","stages":12,"spread":4,"centre":1200,"rate":0.12,"feedback":40}}
```

Envelope funk on guitar or clav:

```json
{"params":{"stages":6,"centre":250,"range":0,"feedback":55,"envelope":3.5,"attack":4,"release":150}}
```

A sweep over two bars, in time:

```json
{"params":{"sync":"bars2","stages":8,"centre":700,"range":4,"feedback":35,"shape":"triangle"}}
```

On a bass or a whole mix, keep the lows whole:

```json
{"params":{"stages":4,"centre":500,"range":2.5,"rate":0.4,"feedback":30,"keep_below":150}}
```

Factory patches: Swirl (the defaults), Classic Four, Resonant Sweep,
Hollow Twelve, Photocell Throb, Lamp Vibrato, Endless Rise, Falling
Spiral, Envelope Funk, Two-Bar Sweep, Stepped Notches, Bass Phase.

## Limits

At `mix` 50 the peaks between notches are at the dry level, so the sound
is a little quieter overall (1 to 3 dB on most material); most of the
level feedback adds is taken back, so turning it up keeps the level near
where it was while the peaks ring. In `rise` and `fall` the dry sound too
passes the 90-degree pair's all-pass (its level is untouched, its phase
shifted, as in an analog frequency shifter), and the wet sound is shifted
by `rate` Hz. The loop rounds over above −4 dBFS. Latency is zero; the
tail Pollia reports follows the feedback and the lowest stage.

See [stock spec §12](../../docs/specs/stock-effects.md#12-phaser--pollia-pollia).
