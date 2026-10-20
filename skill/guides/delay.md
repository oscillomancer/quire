# Bracken: echoes

Use `com.suppleaudio.bracken` for echoes: slapback, rhythmic repeats in
time with the song, ping-pong, tape and analog echo, pitch-shifted,
reversed or smeared repeats, ducked throws and held loops. Patches use
`supple.bracken-patch/1.0`, with physical values under `params`; a merge
changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `sync` | `free`, `n32`, `n16t`, `n16`, `n16d`, `n8t`, `n8`, `n8d`, `n4t`, `n4`, `n4d`, `n2t`, `n2`, `n2d`, `n1` (`t` triplet, `d` dotted) | n4 |
| `time` | ms, 1…5000: used when `sync` is `free` | 400 |
| `groove` | %, −50…50: every second echo later (+) or earlier, each pair on the beat | 0 |
| `offset` | %, −50…50: the right side's times against the left's | 0 |
| `time_changes` | `bend` (glides, bending pitch like tape) or `fade` (crossfades, no bend) | bend |
| `feedback` | %, 0…110: each echo's level against the one before | 40 |
| `routing` | `stereo`, `ping_pong` (the sum bounces left, right, left), `mono` (the sum, centred) | stereo |
| `low_cut` | Hz, 20…2000 in the loop: each echo thinner; 20 is off | 80 |
| `high_cut` | Hz, 1000…20000 in the loop: each echo darker; 20000 is off | 9000 |
| `diffusion` | %, 0…100: each echo blurred more than the last, to a wash | 0 |
| `shift` | semitones, −12…12: each echo's pitch from the one before | 0 |
| `reverse` | bool: every echo backwards, in slices as long as the time | false |
| `medium` | `clean`, `tape`, `analog` (a bucket brigade) | clean |
| `age` | %, 0…100: the medium's wear (below) | 30 |
| `drive` | dB, 0…24: saturation in the loop through the medium's curve | 0 |
| `modulation` | %, 0…100: the time wanders up to ±4 ms | 0 |
| `mod_rate` | Hz, 0.05…10 | 0.8 |
| `duck` | dB, 0…24: the echoes down by this while the input plays | 0 |
| `freeze` | bool: hold the loop forever, the input shut out | false |
| `width` | %, 0…200: the echoes' width | 100 |
| `input` | dB, −48…12: how much input enters the echoes; −48 is closed | 0 |
| `mix` | %, 0…100: the dry stays full to 50%, the echoes are full from 50% | 25 |
| `output` | dB, −24…24 | 0 |
| `echo_time` | ms, readonly: the time between echoes now, at the song's tempo | |
| `decay` | ms, readonly: how long the echoes take to fall 60 dB (at 1 kHz); 600000 when held or growing | |
| `duck_now` | dB, readonly: the ducking this moment | |
| `echo_level`, `input_level`, `output_level` | dBFS, readonly: peaks, falling 20 dB/s | |

Note values follow the host's tempo (120 BPM when there is none): at 120
BPM `n4` is 500 ms, `n8d` 375 ms, `n8t` 167 ms. Each echo goes through the
loop once more than the one before, so `low_cut`, `high_cut`, `shift`,
`diffusion`, `drive` and the medium all build up repeat by repeat.

## The media

| `medium` | What `age` does | Sound |
|---|---|---|
| `clean` | 0: transparent. Up: the band narrows to 8 kHz and the bits fall to 11, like an early digital unit | exact copies |
| `tape` | the highs roll off sooner (19 kHz down to 5.7 kHz) and it wobbles: wow and flutter even with `modulation` at 0 | soft, rounded, unsteady |
| `analog` | fewer bucket-brigade stages: darker and coarser. Longer times are always darker (at 500 ms and 30%, a band to about 3 kHz) | dark, warm, gritty |

`drive` keeps quiet echoes at their level and flattens loud ones from
that many dB below full scale: at 24 dB a runaway loop settles near
−20 dBFS rather than 0.

## Setting it by numbers

Read `echo_time` and `decay` after a change. Decay is what most requests
mean by "shorter" or "longer echoes":

| Aim | `decay` | Typical setting |
|---|---|---|
| A single slap | one echo | `feedback` 0–10 |
| Short rhythmic tail | 1–2 s | `feedback` 25–40 at an eighth or quarter |
| Long, washy | 4–8 s | `feedback` 60–75, `diffusion` 40+ |
| Endless | 600 s (held or growing) | `feedback` 100+, or `freeze` |

Decay grows with the time as well as the feedback: halving the time at the
same feedback halves it. Cuts and age shorten it a little (they take level
each pass).

## Recipes

A vocal slap, tape-soft:

```json
{"params":{"sync":"free","time":95,"feedback":8,"low_cut":150,"high_cut":6500,"medium":"tape","age":20,"mix":18}}
```

Dotted-eighth throws that stay out of the vocal's way:

```json
{"params":{"sync":"n8d","feedback":45,"low_cut":180,"high_cut":5500,"duck":10,"mix":30}}
```

Swung ping-pong (a third: triplet swing):

```json
{"params":{"sync":"n8","routing":"ping_pong","groove":33,"feedback":50,"high_cut":7000}}
```

A wide double (no repeats, the right side half as late again):

```json
{"params":{"sync":"free","time":14,"offset":50,"feedback":0,"low_cut":120,"modulation":25,"mod_rate":0.6,"mix":30}}
```

Shimmer (each echo an octave up, blurring into a wash):

```json
{"params":{"sync":"n4","feedback":65,"shift":12,"diffusion":70,"low_cut":300,"high_cut":10000,"time_changes":"fade","mix":30}}
```

Reversed swells:

```json
{"params":{"sync":"n2","feedback":35,"reverse":true,"diffusion":30,"mix":40}}
```

A dub spiral (feedback at the edge; automate `feedback` past 100 to let it run away):

```json
{"params":{"sync":"n4d","feedback":98,"medium":"tape","age":45,"drive":9,"low_cut":220,"high_cut":3500,"modulation":10,"mix":35}}
```

On a send return, set `mix` to 100.

## Throws and freezes

To echo only some words, automate `input`: −48 (closed) normally, 0 dB
on the words to throw. The echoes already in the loop carry on; the dry
sound is untouched. Freeze holds whatever is in the loop exactly, forever,
and ignores new input: set it just after the sound to hold, and release it
before a render that should end.

## Limits

What feeds back is held under full scale (the first echo is not), and the
echoes leave through a ceiling that eases peaks above −2 dBFS toward 0 dBFS,
so a runaway peaks at 0 dBFS before `mix` and `output`. With `mix` 50% or more the dry and the
echoes sum: watch `output_level`. Diffusion needs a time of 24 ms or more.
The pitch shift is a two-head shifter: a sustained tone's echoes carry a
slight 17 Hz roughness. The tail Bracken reports follows the time, feedback
and tempo, so renders end when the echoes have died; held or growing loops
never end. Latency is zero.

See [mixing](mixing.md) and
[stock spec §6](../../docs/specs/stock-effects.md#6-delay--bracken-bracken).
