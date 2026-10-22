# Withy: chorus, ensemble and vibrato

Use `com.suppleaudio.withy` for anything made of a sound's copies swaying
in pitch: a classic chorus, a subtle stereo widening, a string ensemble's
shimmer, a thick multi-voice double, or a vibrato. Patches use
`supple.withy-patch/1.0`, with physical values under `params`; a merge
changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `style` | `classic`, `dimension`, `ensemble`, `multi`, `vibrato` (below) | classic |
| `voices` | 2…8: how many voices `multi` sings with | 4 |
| `delay` | ms, 1…40: how late the voices are at rest; not with `vibrato` | 7 |
| `width` | %, 0…200: the voices' stereo width | 100 |
| `sync` | `free`, `bars4`, `bars2`, `bar1`, `n2`, `n4`, `n8`, `n16`, `n32`, `n4t`, `n8t`, `n16t`, `n4d`, `n8d` | free |
| `rate` | Hz, 0.03…12: used when `sync` is `free` | 0.6 |
| `depth` | cents, 0…100: each voice's pitch swing either way, the same at any rate | 8 |
| `shape` | `sine` (eases through each swing) or `triangle` (holds sharp, then flat) | triangle |
| `shimmer` | %, 0…100: a fast flutter (about 6 Hz) on top, as a share of the depth | 0 |
| `drift` | %, 0…100: a slow random wander per voice, as a share of the depth | 0 |
| `bloom` | ms, 0…2000: hold the voices still at each new note, then let them sway again over this; 0 is off; free rate only | 0 |
| `medium` | `clean` or `bbd` (an analog bucket brigade: darker the longer the delay, a slight lean and hiss) | clean |
| `age` | %, 0…100: the bucket brigade's wear: darker, more lean, more hiss | 30 |
| `high_cut` | Hz, 1000…20000 on the voices; 20000 is off | 20000 |
| `low_keep` | Hz, 20…1000: below this the sound stays dry and centred; 20 is off | 20 |
| `feedback` | %, −80…80: a ringing, flanged edge (negative is hollow); the level is held | 0 |
| `mix` | %, 0…100: 50 is the classic even blend, at the dry's loudness; not with `vibrato` | 50 |
| `output` | dB, −24…24 | 0 |
| `detune` | cents, readonly: the largest voice's pitch swing now (depth, shimmer and drift together) | |
| `sweep` | ms, readonly: how far the furthest voice's delay swings either way | |
| `rate_now` | Hz, readonly: the sway's rate, synced or free | |
| `mono_loss` | dB, readonly: how much quieter the output folds to mono than the input does | |
| `input_level`, `output_level` | dBFS, readonly: peaks, falling 20 dB/s | |

The depth is a pitch, so changing the rate keeps the detune: a faster rate
sweeps a shorter delay. Very slow rates with a large depth would need more
than 25 ms of sweep; then the voices sweep 25 ms and `detune` reads what
they reach. Read `detune` and `sweep` after a change.

## The styles

| `style` | Voices | Sound | Mono |
|---|---|---|---|
| `classic` | two, half a cycle apart, left and right | the familiar stereo chorus | loses about 1 dB |
| `dimension` | two, each side with the other's turned over | the widest, with the shallowest combs: space more than wobble | the voices partly cancel, leaving mostly the dry (about −2 dB) |
| `ensemble` | three, a third of a cycle apart, left, centre and right | with `shimmer` 40–70 %, a string machine's ensemble | loses under 1 dB |
| `multi` | 2–8, each at its own delay (0.65–1.35 × `delay`), rate and place | thick, unsynchronised doubling; with `drift`, a choir | loses under 0.5 dB |
| `vibrato` | one, on both sides, no dry | pitch vibrato; the mix and delay do not apply | unchanged |

## Recipes

A classic chorus on keys or guitar (the defaults, here in full):

```json
{"params":{"style":"classic","delay":7,"rate":0.6,"depth":8,"shape":"triangle","mix":50}}
```

Width without an audible wobble, for a vocal or a mix bus:

```json
{"params":{"style":"dimension","delay":5,"rate":0.25,"depth":3,"shape":"sine","mix":50}}
```

A string-machine ensemble:

```json
{"params":{"style":"ensemble","delay":8,"rate":0.6,"depth":6,"shimmer":60,"medium":"bbd","age":30,"high_cut":9000,"mix":60}}
```

A thick double for a lead vocal or a pad (bass kept solid):

```json
{"params":{"style":"multi","voices":6,"delay":20,"rate":0.3,"depth":5,"shape":"sine","drift":60,"low_keep":150,"mix":45}}
```

Chorus on bass, the low end dry and centred:

```json
{"params":{"delay":6,"rate":0.5,"depth":7,"low_keep":180,"mix":50}}
```

A singer's vibrato that starts each note in tune and grows:

```json
{"params":{"style":"vibrato","rate":5.2,"depth":30,"shape":"sine","drift":15,"bloom":450}}
```

A slow, flanged swirl in time with the song (a bar a cycle):

```json
{"params":{"sync":"bar1","delay":2.5,"depth":12,"shape":"sine","feedback":55,"mix":50}}
```

On a send return, set `mix` to 100.

## Bloom

Bloom watches the input for new notes. At each one the voices stop
swaying within 4 ms and hold still for 20 ms, so the attack is in tune,
then their motion returns over `bloom` ms. On a chorus it keeps strums and
plucks clean; on `vibrato` it makes the late vibrato of a singer or a
string player. Sustained material with no new notes sways as usual. It
needs a free rate (`sync` `free`).

## Limits

Latency is zero. The voices read up to 25 ms either side of their rest and
at least 1 ms late. Feedback near ±80 % rings at the comb's peaks: on
full-scale input, peaks can pass 0 dBFS; watch `output_level`. The
bucket brigade's hiss follows the signal, so silence stays silent. Mono
in, the voices' two sides are summed. A tempo sync follows the host's
tempo and bar (120 BPM, 4/4 when there is none) and settles onto the song's
position over about a second after a locate.

See [mixing](mixing.md) and
[stock spec §11](../../docs/specs/stock-effects.md#11-chorus--withy-withy).
