# Gorse: transient shaper

Use `com.suppleaudio.gorse` to change the shape of hits: more or less
onset (punch, snap, pick) and more or less tail (ring, room, sustain).
Patches use `supple.gorse-patch/1.0`, with physical values under `params`;
a merge changes only the keys it names.

It works the same at any level: the detector compares two followers in
decibels, so a hit 20 dB quieter is shaped the same, and a steady sound
(a held note, a bass line, a voice) is left alone. There is no threshold
to set.

- **Onset.** While the level is rising faster than it has been, the hit is
  starting: `attack` applies, in full once the rise leads by 10 dB at 50 %
  sensitivity.
- **Tail.** While the level falls faster than it has been, the hit is
  decaying: `sustain` applies, in full once the fall leads by a third of
  that. Fast decays (drums) get it in full; slow ones (a piano) need more
  `length` or `sensitivity`.
- **Lookahead.** The gain lands exactly on each onset, from its first
  sample (1 ms of latency, 1.1 ms with the clip's filters: 52 samples at
  48 kHz). A cut softens the click itself.

| Key | Unit and range | Default |
|---|---|---|
| `attack` | dB, −12…12: the change at a clear onset; negative softens | 0 |
| `sustain` | dB, −12…12: the change through a clear decay; negative shortens | 0 |
| `length` | ms, 1…100, logarithmic: how long an onset lasts (about 1 to 3.5 × this) and how slow a decay still counts | 10 |
| `sensitivity` | %, 0…100: how small a rise or fall counts in full; 0 shapes nothing | 50 |
| `focus` | full, low, mid, high: what the detector hears (below 150 Hz; 250 Hz to 4 kHz; above 5 kHz). The whole signal is shaped | full |
| `clip` | bool: a soft clip last of all; nothing under −6 dBFS changes and nothing passes 0 dBFS | false |
| `mix` | %, 0…100: shaped against dry, in time | 100 |
| `output` | dB, −24…24: level before the clip | 0 |
| `gain_change` | dB, readonly: the largest change in the last block | |
| `onset_change` | dB, readonly: how much the recent hits' onsets changed, at the most | |
| `tail_change` | dB, readonly: how much the recent hits' tails changed, on average | |

After a render, `onset_change` and `tail_change` say what the settings did
to the hits that played (an average that favours the latest few). If
`onset_change` is far under `attack`, the hits are not clear enough to the
detector: raise `sensitivity`, or `focus` on the instrument.

## Recipes

Punchier drums, a little tighter:

```json
{"params":{"attack":6,"sustain":-2}}
```

Kick body on a drum bus, ignoring snare and hats (a loud bass under the
kick hides part of its rise: more sensitivity):

```json
{"params":{"attack":6,"length":15,"focus":"low","sensitivity":70}}
```

Snare crack, shorter ring:

```json
{"params":{"attack":7,"sustain":-3,"length":6,"focus":"mid"}}
```

Dry up a roomy kit:

```json
{"params":{"sustain":-9,"length":12,"sensitivity":60}}
```

Soften a sharp pick or a clicky kick:

```json
{"params":{"attack":-6,"length":5}}
```

Bring out room and decay:

```json
{"params":{"attack":-2,"sustain":6,"length":30,"sensitivity":70}}
```

Raised onsets add peak level: turn `output` down by about the attack, or
turn `clip` on to catch what passes 0 dBFS.

## Measuring it

Render and `compare` the first 10 ms of hits and their decay separately;
peak level rises with attack while loudness barely moves, so compare at
matched loudness. Listen for the cost: a sustain boost lifts room noise
and bleed with the tails, and a large attack on a full mix lifts whatever
plays at each hit.

## Limits

One stereo-linked gain; no mid/side or per-band shaping (`focus` chooses
what is heard, not what is shaped). `low` focus hears the kick through a
filter about 2 ms late, so its gain shapes the body more than the click.
A chord's beating is real movement and moves the gain a little at
extreme settings. The clip guarantees sample peaks, not true peaks; for a
mastering ceiling use [Treeline, the limiter](limiter.md).

See [mixing](mixing.md), [compression](comp.md) and
[stock spec §4](../../docs/specs/stock-effects.md#4-transient-shaper--gorse-gorse).
