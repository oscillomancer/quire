# Flint (`transient`) — transient shaping

Find the instance id in `project.get`, then use `fx.describe` to read its
physical schema. Apply the flat parameter keys below with `fx.patch.merge`
(`{"fx":"fx_1","patch":{"params":{"attack":60}}}`).
`fx.patch.get` captures a reproducible setting. Render, analyze and compare
before keeping a change; increase onset or tail by sound, not by peak alone.
Plugin id: `com.suppleaudio.transient`; patch schema:
`supple.transient-patch/1.0`. There is no random seed or sidechain.

| Key | Physical unit / type | Range | Default | Meaning |
|---|---|---|---|---|
| `attack` | % | -100..100 | 0 | Onset boost/cut, up to +/-12 dB |
| `sustain` | % | -100..100 | 0 | Decay boost/cut, up to +/-12 dB |
| `speed` | ms, log | 1..50 | 10 | Fast release and slow attack |
| `sensitivity` | % | 0..100 | 50 | Scale envelope difference; zero stops shaping |
| `clip` | bool | false / true | false | 2x tanh soft clip; restart, not automatable |
| `mix` | % | 0..100 | 100 | Aligned dry/wet blend |
| `output` | dB | -24..24 | 0 | Final gain after clip and mix |
| `input_level` | readonly dBFS | -120..40 | -120 | Input sample peak every 50 ms |
| `output_level` | readonly dBFS | -120..40 | -120 | Output sample peak every 50 ms |
| `gain` | readonly dB | -12..12 | 0 | Mean shaping gain every 50 ms |

Recipes (the JSON is the `patch` argument for `fx.patch.merge`):

- **Punch with a shorter tail:**
  `{"params":{"attack":60,"sustain":-30,"speed":10,"sensitivity":50,"clip":false,"mix":100,"output":-3}}`.
  Compare the first 5 ms and decay separately; output trim preserves headroom.
- **Soften a sharp pick:**
  `{"params":{"attack":-40,"sustain":0,"speed":5,"sensitivity":50,"clip":false,"mix":75,"output":0}}`.
  Short speed follows brief onsets; mix restores some dry definition.
- **Bring out room/decay:**
  `{"params":{"attack":0,"sustain":45,"speed":20,"sensitivity":65,"clip":false,"mix":70,"output":-3}}`.
  Listen for unwanted bleed and room noise as well as the useful sustain.

The stereo maximum detector drives one gain, preserving stereo balance.
A peak hold tracks carrier maxima (1..50 ms, up to roughly half a cycle);
fast and slow dB followers separate rises from falls. Attack is not an
attack time, sustain is not a release time, and speed controls both detector
branches. The dB difference makes the shaping level-independent, including
its initial onset. One part acts at a time, with total gain bounded to
+/-12 dB and a 1 ms gain pole. Neutral defaults pass bit-exactly. Continuous
changes smooth for 20 ms; setting a patch before playback snaps its values.

Clip off is zero latency. Clip on adds exactly 128 samples, aligned with
the dry path, through a local 2x half-band pair. Toggle it while stopped or
let the host service the requested restart. Clip is a soft tone change;
it does not guarantee a sample or true-peak ceiling, and output gain can
raise peaks afterwards. Use Ballast limiting for a mastering ceiling.

This is a linked broadband effect: it cannot isolate a snare from bass or
cymbals in a full mix, and it has no external key or frequency selectors.
Slow low-frequency carriers can delay decay recognition through peak hold.
The relative detector floor is 120 dB below the largest level since reset;
material below that floor is treated as silence. Tanh is anti-aliased by 2x
filtering but is not claimed alias-free at arbitrary drive/frequency.
GUI histories and readonly meters describe the signal, not loudness targets.

See [mixing](mixing.md), [compression/limiting](comp.md),
[stock spec §4](../../docs/specs/stock-effects.md#4-transient-shaper--flint-transient)
and [implementation §9](../../docs/specs/effects.md#9-second-wave-transient-shaper--flint-transient-stock-effectsmd-4).
