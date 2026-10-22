# Ghyll: rooms

Use `com.suppleaudio.ghyll` for any acoustic space: a vocal booth, a
drum room, a hall, a church, a stairwell, a courtyard. You build the room:
its size in metres, what its walls, floor and ceiling are made of, where
the sound stands and where you listen. Reflections, decay, bass and level
follow from the physics, so set it the way you would describe a real
place. Patches use `supple.ghyll-patch/1.0`, with physical values under
`params`; a merge changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `room_width`, `room_depth`, `room_height` | m: 1.5…60, 1.5…100, 2…40 (width left to right, depth front to back) | 17, 25, 11 |
| `walls`, `floor`, `ceiling` | `stone`, `concrete`, `brick`, `plaster`, `wood`, `glass`, `carpet`, `curtains`, `audience` (seated people or seats), `acoustic` (treated panels), `open` (nothing there: the sky, an open side) | `plaster`, `audience`, `wood` |
| `decay` | %, 25…400: the reverberation time against the room as built; 200 doubles every octave's | 100 |
| `scatter` | %, 0…100: rough surfaces; 0 gives clear, discrete reflections (and flutter), 100 blurs them | 40 |
| `source_x`, `source_y` | %, 0…100: where the sound stands, across from the left wall and back from the front wall | 44, 22 |
| `listener_x`, `listener_y` | %, 0…100: where you listen (facing the source) | 54, 44 |
| `mic` | `ortf` (a natural stereo pair), `spaced` (omnis 60 cm apart: wide, airy), `head` (as heard there, for headphones), `mono` | ortf |
| `predelay` | ms, 0…250: a gap added before the room answers | 0 |
| `early`, `late` | dB, −30…12: the reflections and the tail against the room's own level; −30 is off | 0, 0 |
| `low_cut`, `high_cut` | Hz: 20…1000 (20 off), 1000…20000 (20000 off), on the room only | 20, 20000 |
| `width` | %, 0…200: the room's stereo width | 100 |
| `freeze` | bool: hold the tail forever, let nothing new in | false |
| `mix` | %, 0…100: the dry stays full to 50%, the room is full from 50%; 100 is the room alone | 25 |
| `output` | dB, −24…24 | 0 |
| `rt60`, `rt60_low`, `rt60_high` | ms, readonly: the time the room takes to fall 60 dB at 500 Hz–1 kHz, at 125 Hz, at 4 kHz | |
| `distance`, `critical_distance` | m, readonly: source to listener; where the room grows as loud as the direct sound | |
| `first_reflection` | ms, readonly: the gap from the direct sound to the first reflection, predelay included | |
| `clarity` | dB, readonly: C80, the first 80 ms against the rest. Music sounds clear above 0; concert halls run −3 to +3 | |
| `definition` | %, readonly: D50, the share in the first 50 ms. Speech is clear above 50% | |
| `input_level`, `room_level`, `output_level` | dBFS, readonly: peaks, falling 20 dB/s | |

## How the room behaves

- **Size** sets when reflections arrive and how dense the tail is; volume
  and surface area set how long it lasts. A large room darkens as it
  decays because the air absorbs the highs over long paths.
- **Materials** set each octave's decay. Hard surfaces (stone, concrete,
  brick, glass) ring; carpet and curtains take the highs; wood takes the
  lows; an audience or acoustic panels take nearly everything. Read
  `rt60`, `rt60_low` and `rt60_high` after a change.
- **Distance** sets the balance. Near the source (inside
  `critical_distance`) the direct sound dominates and `clarity` is high;
  farther back the room takes over. The room's level against the dry is
  physical: at `mix` 50 it is the balance heard at the listener's place.
- **Small rooms** have audible standing waves (modes) below a few hundred
  hertz: the bass changes with where the source and listener stand. In the
  middle of the room the lowest modes cancel; in a corner they all sound.

## Setting it by numbers

To reach a reverberation time, read `rt60` and scale `decay` by
target ÷ reading (every octave scales together): at `rt60` 1330 ms, a
2 s target is `decay` 150. To make a hall sound farther away, move
`listener_y` toward 100 and watch `clarity` fall; to bring a voice
forward without shrinking the room, add `predelay` (20–40 ms) or lower
`late`.

| Space | Typical RT60 | Setting |
|---|---|---|
| Vocal booth, treated room | under 0.2 s | small, `acoustic` walls and ceiling, `carpet` floor |
| Living room, studio live room | 0.4–0.7 s | 5–10 m, mixed materials, `decay` to taste |
| Recital or concert hall | 1.3–2.2 s | 15–30 m wide, `audience` floor, `plaster` or `wood` walls |
| Church | 3–5 s | `stone` walls, `wood` floor (pews) |
| Cathedral, stairwell, garage | 3–10 s | hard surfaces throughout |

## Factory patches

Recital Hall (the defaults), Concert Hall, Back of the Hall, Hall on
Headphones, Small Church, Cathedral, Live Room, Drum Room, Vocal Booth,
Living Room, Tiled Bathroom, Echo Chamber, Stairwell, Courtyard, Parking
Garage, Warehouse. Each sets its own `mix` so the room sits 6 to 9 dB
under the dry sound (a far seat or a hard room needs a small mix).

## Recipes

A vocal in a warm hall, clear of the words:

```json
{"params":{"room_width":20,"room_depth":30,"room_height":12,"walls":"wood","floor":"audience","ceiling":"plaster","predelay":30,"low_cut":150,"mix":20}}
```

A tight drum room, the mics close:

```json
{"params":{"room_width":7,"room_depth":9,"room_height":4,"walls":"brick","floor":"wood","ceiling":"plaster","decay":45,"listener_y":60,"scatter":35,"mic":"spaced","mix":25}}
```

A guitar in a stone church, from the back pews:

```json
{"params":{"room_width":12,"room_depth":30,"room_height":12,"walls":"stone","floor":"wood","ceiling":"plaster","decay":55,"listener_y":85,"mix":15}}
```

An open courtyard (no ceiling):

```json
{"params":{"room_width":20,"room_depth":20,"room_height":15,"walls":"brick","floor":"stone","ceiling":"open","mix":30}}
```

On a send return set `mix` to 100 and balance with the send level; the
room's level then still follows the listener's distance.

## Limits

The room is a box: no balconies, pillars or curved walls. Reflections are
modelled up to three bounces; beyond that the tail takes over. `clarity`
and `definition` are an omni's at 1 kHz. Freeze holds the tail exactly;
new sound is shut out until it is released. The tail Ghyll reports
follows the room, so renders end when it has died (never while frozen).
Latency is zero.

See [mixing](mixing.md) and
[stock spec §7](../../docs/specs/stock-effects.md#7-reverb--ghyll-ghyll).
