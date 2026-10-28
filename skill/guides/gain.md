# Gain: a level change in the chain

Use `com.suppleaudio.gain` where a level has to change at one point in an
FX chain: before a plugin that reacts to level (a saturator, a compressor,
a gate) to drive it harder or softer, after one to make up or trim, or to
match two chains' levels before comparing them. A track's own volume is its
fader (`track.set`); Gain is for the places a fader cannot reach. For
polarity, width, mono checks or a mute, use Hickory (`guides/utility.md`).

It has one parameter. Patches use `supple.gain-patch/1.0`, with the value
under `params`:

| Key | Unit and range | Default |
|---|---|---|
| `gain` | dB, −32…32 | 0 |

At 0 dB the output is the input, bit for bit. It adds no latency and no
tail, and works on mono and stereo tracks alike. A change glides (a 20 ms
smoothing, in dB), so automating it or jumping it never clicks; the glide
starts at the sample the change is at, and a render is the same at any
block size. Its factory patch list is empty: 0 dB is its only neutral
setting.

## Recipes

Push a saturator 6 dB harder, then bring the level back after it (two Gain
instances around it):

```json
{"params":{"gain":6}}
```

```json
{"params":{"gain":-6}}
```

Trim a hot plugin's output by 3.5 dB:

```json
{"params":{"gain":-3.5}}
```

A level ride at one point in a chain: automate `gain` with an envelope
(`env.add` on the parameter), as with any plugin parameter.
