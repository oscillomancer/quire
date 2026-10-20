# Hickory: stereo utility

Use `com.suppleaudio.hickory` for the plain jobs: gain staging, a polarity
flip, checking a mix in mono or hearing only its side, narrowing or
widening, making the lows mono, removing DC, and delaying a track to line
it up with another. Patches use `supple.hickory-patch/1.0`, with physical
values under `params`; a merge changes only the keys it names.

At rest it changes nothing (the output is the input, bit for bit), and it
adds no latency. Every switch fades over 10 ms, so all of it can be
automated.

| Key | Unit and range | Default |
|---|---|---|
| `gain` | dB, −60…24; −60 is silence | 0 |
| `mute` | bool | false |
| `delay` | ms, 0…50, to the nearest sample; not reported as latency | 0 |
| `channels` | stereo, swap, left, right, mono, side | stereo |
| `width` | %, 0…200: the side's level; 0 is mono | 100 |
| `balance` | %, −100…100: positive lowers the left, negative the right | 0 |
| `invert_left`, `invert_right` | bool: polarity, before the routing | false |
| `bass_mono` | bool: the side below `mono_below` removed | false |
| `mono_below` | Hz, 30…500: bass mono's crossover | 120 |
| `dc_block` | bool: a 12 dB/oct high-pass at 5 Hz | false |
| `output_peak` | dBFS, readonly: the highest sample peak of the last 3 s | |
| `correlation` | readonly, −1…1: 1 mono, 0 unrelated sides, −1 one side inverted; 0 in silence | |
| `level_balance` | dB, readonly: how much louder the right is than the left | |
| `side_level` | dB, readonly: the side against the mid; −60 is mono, 0 as much side as mid | |

The order is polarity, channels, DC block, delay, width and bass mono,
balance, then gain and mute. `mono` and `side` put (L+R)/2 or (L−R)/2 on
both outputs; width and bass mono act on the side only, so the mono sum
never changes. On a mono track only gain, mute, `invert_left`, the DC block
and the delay apply.

The readouts cover the last 3 s of output: render at least that much, then
read them with `param_get`.

## Recipes

Leave headroom before a saturator or a bus:

```json
{"params":{"gain":-6}}
```

Check a mix in mono, then go back:

```json
{"params":{"channels":"mono"}}
```

A wide synth bass that should stay solid in mono (`side_level` falls; the
mono sum is untouched):

```json
{"params":{"bass_mono":true,"mono_below":150}}
```

A little wider without the lows spreading:

```json
{"params":{"width":130,"bass_mono":true,"mono_below":150}}
```

A stereo pair with one side wired backwards: `correlation` near −1 on
material that should be centred. Flip one side and compare:

```json
{"params":{"invert_right":true}}
```

Line a DI up with its amp mic (the DI arrives first; 1 ms is about 34 cm
of distance): find the offset by comparing the two renders' onsets, then
delay the earlier one:

```json
{"params":{"delay":1.4}}
```

## Measuring it

Polarity and width changes shift the balance between the mid and the side;
compare loudness of stereo and mono renders before and after. Bass mono
leaves the mid alone, so a mono render does not change at all; listen to
the side (`channels: side`) to hear what it took.

## Limits

No panning law for a mono source (`balance` turns one side down), no
fractional delay, no limiter or loudness normalization (see
[Treeline](limiter.md)). The 24 dB/oct crossover leaves some side below it:
at 150 Hz, the side at 50 Hz is 38 dB down, not gone. The DC block is
0.02 dB down at 20 Hz. `output_peak` is a sample peak, not a true peak.

See [mixing](mixing.md) and
[stock spec §8](../../docs/specs/stock-effects.md#8-utility--hickory-hickory).
