# Delay: Echelon (`delay`)

Use Echelon for rhythmic echoes, stereo movement, tape-style timing glides,
wet ducking and held loops. Patch schema: `supple.delay-patch/1.0`; values
are physical units and enums are keys. Changes can be merge patches through
`fx.patch.merge`; inspect the generated schema for the installed version.

| Key | Unit/range or choices | Default |
|---|---|---|
| `sync` | `off`, `n8_1`, `n4_1`, `n2_1`, `n1_1`, `n1_2`, `n1_2d`, `n1_2t`, `n1_4`, `n1_4d`, `n1_4t`, `n1_8`, `n1_8d`, `n1_8t`, `n1_16`, `n1_16d`, `n1_16t`, `n1_32` | `n1_8d` |
| `time` | ms, 1–5000, log; used with sync off | 375 |
| `mode` | `stereo`, `ping_pong`, `mono` | `stereo` |
| `offset` | %, −50–50; right time relative to left | 0 |
| `feedback` | %, 0–110 | 35 |
| `highpass` | Hz, 20–2000, log | 80 |
| `lowpass` | Hz, 1000–20000, log | 8000 |
| `mod_rate` | Hz, 0.05–10, log | 0.5 |
| `mod_depth` | %, 0–100 | 0 |
| `drive` | dB, 0–24 | 0 |
| `duck` | dB, 0–24; maximum wet attenuation | 0 |
| `width` | %, 0–200, echo only; ignored in mono | 100 |
| `freeze` | bool | false |
| `mix` | %, 0–100, linear dry/wet | 25 |
| `output` | dB, −24–24 | 0 |

Note labels follow Orrery: `n1_8d` displays `1/8.`, a dotted eighth;
`t` means triplet. At 120 BPM the dotted eighth is 375 ms. Missing transport
uses 120 BPM. Both times clamp to 1–5000 ms, including long notes and offsets.

Three starting recipes (apply the `params` object as a merge):

```json
{"params":{"sync":"off","time":95,"feedback":10,"highpass":120,"lowpass":6500,"mix":15}}
```

A short vocal slap; keep feedback low so consonants remain clear.

```json
{"params":{"sync":"n1_4","mode":"ping_pong","feedback":55,"mod_depth":30,"mix":25}}
```

Quarter-note alternating echoes. Ping-pong feeds the mono sum into the left
line; the first echo is left, then right. Offset changes the right spacing.

```json
{"params":{"sync":"n1_8d","feedback":40,"duck":12,"highpass":180,"lowpass":6000,"mix":30}}
```

Ducked rhythmic throws: the wet echo falls by 12 dB under input above
−30 dBFS and recovers with a 250 ms detector release. Below −60 dBFS there
is no ducking. Duck never alters stored repeats.

Time and tempo changes glide with two 100 ms time constants, bending pitch.
Wow combines a sine with deterministic smooth random variation; maximum
excursion is 3 ms, capped at 45% of the current line time. Zero depth is exact.
The filters sit in the loop and therefore darken/thin each repeat; endpoints
remain active. Drive is a small-signal-preserving loop saturator, not makeup.

Freeze fades the input and loop processing out over 10 ms, holds the current
read times and replays the captured period without cumulative interpolation
loss. Its period rounds to the nearest sample while retaining fractional
playback phase. New input is ignored once frozen; new time/tempo targets
apply on unfreeze. Freeze and feedback ≥100% have infinite reported tails;
release freeze or reduce feedback before an offline render that needs to end.

The safety limiter leaves echoes below −6 dBFS unchanged and bounds each
normal echo to 0 dBFS. Wider stereo settings, positive output, dry signal and
frozen material can exceed that bound. It is not a true-peak master limiter.
Use Ballast for delivery ceilings. Latency is zero; no parameter requires a
restart. The GUI pattern is a nominal per-line picture, before width/mix/duck.
