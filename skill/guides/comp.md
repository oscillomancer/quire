# Sundew: compressor

Use `com.suppleaudio.sundew` to control a track's dynamics, glue a bus, duck
a track from an external key, lift quiet detail (upward compression), or
catch peaks. Patches use `supple.sundew-patch/1.0`, with physical values
under `params`; a merge changes only the keys it names.

Pick a `character` first; ratio, attack and release mean the same in each:

| Character | What it is | Reach for it on |
|---|---|---|
| `clean` | Feedforward, peak, exact; no colour | Anything where only the numbers should change; peak control; ducking |
| `glue` | Feedforward RMS; a slow stage makes release program-dependent (short peaks recover fast, long passages slowly) | Drum and mix buses |
| `opto` | Feedback RMS; drops fast then lets the last few dB go over seconds, longer after long compression; big jumps grab faster | Vocals, bass, anything that needs leveling |
| `fet` | Feedback peak, very fast, ratio steepening above the threshold, release quickening when it works hard | Snare and drum attack, aggressive parallel compression |
| `vari_mu` | Feedforward RMS, the ratio growing with the reduction (the ratio set is where it ends up), S-shaped movement | Gentle mix-bus compression, mastering |
| `bloom` | Upward: lifts what is below the threshold, up to `range`, never the noise floor | Room tails, quiet detail, acoustic sources |

| Key | Unit and range | Default |
|---|---|---|
| `character` | clean, glue, opto, fet, vari_mu, bloom | clean |
| `threshold` | dB, −60…0 (Bloom: the level it lifts below) | −18 |
| `ratio` | 1…20, logarithmic | 3 |
| `attack` | ms, 0.05…250, logarithmic | 10 |
| `release` | ms, 5…3000, logarithmic; ignored while `sync` is on | 150 |
| `sync` | off, n32, n16, n8, n4, n2, bar: release recovers within that note at the tempo | off |
| `knee` | dB, 0…24 (Glue, Opto and Vari-Mu add their own) | 6 |
| `range` | dB, 1…40: the deepest cut, or Bloom's highest lift | 24 |
| `key` | internal, external (the sidechain input, when connected) | internal |
| `key_hpf` | Hz, 20…800 high-pass on the key; 20 is off | 20 |
| `link` | %, 0…100: how much the channels share one gain | 100 |
| `stereo` | lr, ms: left/right or mid/side (lower `link` to treat mid and side apart) | lr |
| `lookahead` | off, ms1, ms2, ms5; adds that latency; changing it restarts the plugin | off |
| `listen` | off, key, delta: hear the key, or what the compression removes; for people, not mixes | off |
| `colour` | %, 0…100: the character's harmonics, more as it works harder (Clean, Bloom: none) | 0 |
| `output` | dB, −24…+24, after mix and match | 0 |
| `mix` | %, 0…100: below 100 is parallel compression | 100 |
| `match` | bool: keeps the output as loud as the input, measured over about a second | false |
| `gain_change` | dB, readonly: the largest gain change in the last block (negative cuts) | |
| `loudness_change` | dB, readonly: output against input, before the match | |

## Recipes

Level a vocal, compared at equal loudness:

```json
{"params":{"character":"opto","threshold":-26,"ratio":3,"attack":10,"release":120,"match":true}}
```

Glue a drum bus, keeping the kick from driving it and some of the dry punch:

```json
{"params":{"character":"glue","threshold":-20,"ratio":4,"attack":30,"release":100,"key_hpf":90,"mix":75,"match":true}}
```

Duck a bass from the kick, recovering by the next eighth note: set the key,
then route the kick to it (`route.add {"from":"tr_kick","fx":"fx_bass_comp",
"tap":"post_fader"}`). Without a connected key, Sundew hears its own input.

```json
{"params":{"character":"clean","key":"external","threshold":-30,"ratio":10,"attack":1,"sync":"n8","knee":0,"range":12}}
```

Lift a room's tail and quiet detail without touching the loud hits:

```json
{"params":{"character":"bloom","threshold":-30,"ratio":2.5,"attack":3,"release":250,"range":10}}
```

## Measuring it

Render and read `gain_change` (or the analysis report): crest factor falls
as compression rises; loudness falls unless `match` or `output` makes it up.
`match` exists so a bypass comparison is fair: compare with it on, then
decide whether the result is better, not merely louder. `loudness_change`
says how much quieter the compression made it.

## Limits

Sundew is not a limiter: there is no ceiling and no true-peak detection.
Clean at 20:1 with `lookahead` `ms1` holds sample peaks closely; for a
true-peak ceiling use [Treeline, the limiter](limiter.md). With an external
key, Opto and FET detect the key's level and keep their feedback gain law.
Lookahead delays the audio, not the detector, and the latency changes only
after the host restarts the plugin. Match is a slow gain (about a second);
it follows the song's loudness, so turn it off before printing if the level
should stay put.
