# Umber: tape

Use `com.suppleaudio.umber` for what a tape machine does to a sound: the
faint rounding of a mix's peaks on a well-kept modern machine, a drum bus
pushed into tape, a fatter low end from the head bump, or a worn,
warbling cassette. Patches use `supple.umber-patch/1.0`, with physical
values under `params`; a merge changes only the keys it names.

| Key | Unit and range | Default |
|---|---|---|
| `drive` | dB, −24…24: how hard the signal meets the tape; at 0 dB a sine at −18 dBFS RMS records at the tape's reference level (0 VU) | 0 |
| `tape` | `modern`, `classic`, `vintage`: 12.5, 9 and 5 dB from reference level to 3% distortion, each softer and noisier | modern |
| `bias` | %, 50…150 of the recommended bias: under is brighter and grittier (distortion at low levels too), over darker and cleaner | 100 |
| `auto_gain` | on: the playback level falls as the drive rises, so the level holds | on |
| `speed` | `ips30`, `ips15`, `ips7_5`, `ips3_75` (inches per second) | ips15 |
| `emphasis` | `nab`, `iec`: the equalisation standard; not at 30 ips (AES) | iec |
| `head_bump` | %, 0…200: the playback head's swell in the lows; 100 is a real head's | 100 |
| `wow` | %, 0…2: slow pitch drift as a peak speed error; 0.02 a studio machine, 0.2 a cassette deck | 0.02 |
| `flutter` | %, 0…1: faster pitch waver (capstan, idlers, scrape) | 0.02 |
| `wear` | %, 0…100: worn, dirty, misaligned, magnetized heads: duller highs, the channels apart in time, even harmonics, unsteady speed, dropouts | 0 |
| `hiss` | dB, −30…12 against the real tape's noise at this speed; −30 is off | −30 (off) |
| `output` | dB, −24…24 | 0 |
| `latency` | `full` (about 8.5 ms: oversampling with no phase change, deep wow) or `low` (under 10 samples, for tracking: wow held small); changing it restarts the plugin | full |
| `vu` | dB, readonly: the record level against reference, as a VU meter reads it | |
| `headroom` | dB, readonly: how far the loudest recent peak, as the tape sees it (highs lifted), stayed under the 3% limit; negative is over | |
| `saturation` | dB, readonly: how much the tape took off the loudest recent peaks | |

## Levels: what the drive means

The tape is calibrated as a machine is. A sine at −18 dBFS RMS (−15 dBFS
peak) reads 0 VU and records at reference level, where a modern tape adds
about 0.1% third harmonic. Distortion then rises about 2 dB per dB: 1% at
+9 dB, 3% (the tape's limit) at +12.5 dB, and from there it saturates
quickly. Highs reach the limit sooner, because the record equalisation
lifts them into the tape: at 15 ips a 16 kHz tone saturates 8 to 11 dB
before 1 kHz does (NAB, IEC), and at 7.5 and 3.75 ips much sooner.

So set `drive` by the meters, not by ear alone. Loud passages around
0 VU give the gentle rounding of a well-run session; `headroom` near 0 dB
means the peaks touch the limit; `saturation` says how much they lost.
A mix at about −18 LUFS needs no drive; a mastered track at −10 LUFS needs
about −8 dB to sit in the same place.

## Recipes

The subtle one, for a mix bus or a master: a modern machine at 30 ips,
the peaks just touched, the lows barely coloured. Check `saturation`
reads a fraction of a dB on the loudest moments:

```json
{"params":{"tape":"modern","speed":"ips30","drive":0,"head_bump":60,"wow":0.01,"flutter":0.01}}
```

Rounding peaks without any pitch movement (drums, a bus):

```json
{"params":{"tape":"modern","speed":"ips15","drive":5,"wow":0,"flutter":0}}
```

Hot drums into older tape, NAB for the low end:

```json
{"params":{"tape":"classic","speed":"ips15","emphasis":"nab","drive":9,"head_bump":120,"output":1.5}}
```

A worn machine, and a cassette:

```json
{"params":{"tape":"vintage","speed":"ips7_5","bias":85,"drive":4,"wow":0.25,"flutter":0.15,"wear":70,"hiss":4,"output":3}}
```

```json
{"params":{"tape":"vintage","speed":"ips3_75","drive":-3,"wow":0.12,"flutter":0.1,"wear":20,"hiss":0,"output":3}}
```

Tracking through it live: `"latency":"low"` (it restarts the plugin, so
set it before playing).

Factory patches: Mastering, Mix Bus, Peak Rounding, Tracking, Hot Drums,
Fat Bass, Saturated, Underbiased, Cassette, Worn Machine, Warped, Clean
Transfer (`preset_list`/`fx_patch_list`).

## What each part does

- **Speed** moves the head bump (about 3.8 Hz per ips: 114 Hz at 30 ips,
  57 at 15, 28 at 7.5), saturates the highs sooner as it slows, loses
  more top, and hisses and wavers more.
- **Emphasis** (below 30 ips): NAB lifts the lows into the tape below
  50 Hz, so bass saturates sooner, and records the highs with more
  headroom but plays them back with a little more hiss; IEC the reverse.
- **Bias** at 100% leaves the tape almost free of hysteresis: clean at low
  levels, rounding only near the limit. Lower bias opens the loop: quiet
  sounds turn grainy, the highs lift, and below about 70% everything gets
  quieter. Higher bias darkens and cleans.
- **Auto gain** keeps the quiet level, not the loudness: heavy drive still
  comes out quieter, because the tape takes the peaks off. Trim `output`
  by ear or with an analysis.
- **Hiss** is off by default. At 0 dB it is the real tape's: about
  −78 dBFS A-weighted at 15 ips, with modulation noise riding the signal.

## Limits

- It changes level in the lows (the head bump) even when nothing
  saturates; set `head_bump` to 0 for a flat low end.
- Low latency holds wow to a few samples of sway: deep wow needs `full`.
- Switching `tape`, `speed` or `emphasis` while playing drops out for
  about 15 ms (the machine restarts clear); automate them between notes.

See [stock spec §13](../../docs/specs/stock-effects.md#13-tape--umber-umber).
