# Sorrel: gate and expander

Use `com.suppleaudio.sorrel` to turn down what plays between the sounds you
want: spill between drum hits, hiss or room noise between phrases. Patches
use `supple.sorrel-patch/1.0`, with physical values under `params`; a merge
changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `mode` | `gate` (open or closed), `expand` (quieter sound turned down by `ratio`) | gate |
| `threshold` | dBFS, −80…0: the key level above which it opens | −40 |
| `hysteresis` | dB, 0…24: it closes once the level is this far under the threshold (gate) | 6 |
| `range` | dB, −80…0: the gain while closed; −80 is silence | −40 |
| `ratio` | 1.2…10 (expand): each dB under the threshold becomes this many | 2 |
| `attack` | ms, 0.05…50: how long the opening takes | 1 |
| `hold` | ms, 0…1000: how long it stays open once the level falls under the close level | 20 |
| `release` | ms, 10…3000: how fast it closes, as the time to turn down 60 dB | 150 |
| `follow` | bool: close at the rate the sound was already decaying at, within ¼ to 4 × the release (gate) | true |
| `lookahead` | `off`, `ms1`, `ms2`, `ms5`; adds that latency; a restart, not automatable | ms2 |
| `key` | `internal`, `external` (the sidechain input, when something is routed to it) | internal |
| `key_hpf` | Hz, 20…2000 on what it listens to; 20 is off | 20 |
| `key_lpf` | Hz, 100…20000 on what it listens to; 20000 is off | 20000 |
| `listen` | `off`, `key` (what it listens to), `delta` (what it takes away) | off |
| `gain` | dB, readonly: the gain now (0 open, the range closed) | |
| `noise_floor` | dBFS, readonly: the commonest quiet key level of the last 10 s | |
| `loud_level` | dBFS, readonly: the key level exceeded 5 % of the last 10 s | |
| `suggested_threshold` | dBFS, readonly: a threshold that clears the noise | |
| `open_time` | %, readonly: how much of the last 10 s it was open | |
| `openings` | readonly: how many times it opened in the last 10 s | |

Levels are the key's peak over 10 ms, in dBFS: compare them with the
readouts, not with an RMS loudness. With lookahead the opening ends a
millisecond before the level crosses the threshold, so onsets pass whole.

## Finding the threshold

Render at least 10 s of the track with the gate in place (any settings),
then read the readouts with `param_get`:

1. `noise_floor` is the noise or spill; `loud_level` the sound. Less than
   about 15 dB between them and a gate will not separate them: use Expand,
   or leave it.
2. `suggested_threshold` puts the close level (threshold − hysteresis)
   2 dB above the noise's peaks. Set `threshold` to it.
3. Render again and read `openings`: about one per hit or phrase is right.
   Many more is chatter: raise `hysteresis` or `hold`. Fewer means quiet
   hits stay closed: lower `threshold`, or filter the key.

Spill that is as loud as quiet wanted notes (a kick in the snare mic) is not
noise to the histogram: filter the key so only the wanted sound reaches it,
then read the readouts again (they measure the filtered key).

## Recipes

Snare mic, kick and hats spilling (the high-pass keeps the kick out of the
key, a finite range keeps some room):

```json
{"params":{"threshold":-30,"range":-30,"hold":30,"release":120,"key_hpf":180}}
```

Kick mic, closed tight between hits (the low-pass keeps the snare and hats
out of the key):

```json
{"params":{"threshold":-28,"range":-80,"hold":30,"release":90,"key_lpf":180}}
```

A tom left to ring (Follow fades it at the rate it rings):

```json
{"params":{"threshold":-32,"range":-24,"hold":60,"release":400,"key_hpf":60,"key_lpf":2000}}
```

Hiss between sung phrases, gently (Expand keeps soft onsets and endings):

```json
{"params":{"mode":"expand","threshold":-48,"ratio":2,"range":-18,"hold":120,"release":300,"key_hpf":100}}
```

A pad chopped by a rhythm on another track: route that track to the
sidechain input first.

```json
{"params":{"key":"external","threshold":-30,"range":-80,"attack":0.5,"hold":10,"release":40,"follow":false,"lookahead":"ms5"}}
```

## Measuring it

`listen: "delta"` plays only what the gate removes: render it and check
that it holds no wanted sound (its level during hits should be near the
noise floor). Compare the onsets of a gated and an ungated render: with
lookahead they should match. `open_time` and `openings` change with the
threshold, so read them after each change.

## Limits

A gate cannot remove noise under the sound it lets through; only between
sounds. Soft onsets below the threshold are turned down until the level
crosses it (use Expand, or a lower threshold). The suggestion assumes the
material has quiet stretches. An external key that is not connected falls
back to the input. Lookahead changes restart the plugin.

See [mixing](mixing.md) and
[stock spec §3](../../docs/specs/stock-effects.md#3-gate--expander--sorrel-sorrel).
