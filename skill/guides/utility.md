# Plumb — stereo utility

CLAP id `com.suppleaudio.utility`; artifact `Plumb.clap`. Use Plumb for gain
staging, balance, channel inspection, polarity checks and bass mono.
Patch schema: `supple.utility-patch/1.0`. Values are physical units:
`{"schema":"supple.utility-patch/1.0","params":{"width":150,"mono_below":120}}`.
Use merge patches for adjustments; query the schema rather than guessing ids.

| Key | Unit / values | Range | Default |
|---|---|---|---|
| `gain` | dB | −120..24; −120 is silence (−inf) | 0 |
| `pan` | percent | −100..100; positive attenuates left | 0 |
| `width` | percent | 0..200; side multiplier | 100 |
| `invert_left` | bool | false / true | false |
| `invert_right` | bool | false / true | false |
| `swap` | bool | false / true | false |
| `channels` | enum | stereo, left_to_both, right_to_both, mid, side | stereo |
| `mono_below` | Hz | 0..500; 0 is off | 0 |
| `dc_block` | bool | false / true; 5 Hz high-pass | false |
| `mute` | bool | false / true | false |
| `input_level` | readonly dBFS | −120..40 | −120 |
| `output_level` | readonly dBFS | −120..40 | −120 |
| `correlation` | readonly | −1..1; silence is 0 | 0 |

Three recipes:

1. Leave headroom: merge `{"params":{"gain":-6}}`, render, inspect output
   peak and loudness. This attenuates without limiting peaks.
2. Stabilize a wide bass: merge `{"params":{"mono_below":120,"width":150}}`.
   Compare stereo and mono renders. M stays unchanged; low S is attenuated.
3. Inspect channel problems: merge `{"params":{"channels":"side"}}` to
   audition differences, then restore `stereo`. Test `invert_right:true`
   only when measurements suggest a polarity fault; compare before keeping it.

Linear balance leaves the favoured side unchanged and attenuates the other.
Width 0 produces the average on both sides; 200 doubles S. Swap precedes
channel selection, then polarity, DC block, width/bass mono, pan, gain, mute.
Gain, pan, width and crossover move over 20 ms, switches over 10 ms.

Limits: no limiter, loudness normalization or intentional delay. Bass mono
uses minimum-phase LR4 on S, has zero latency and does not fully remove
bass: 50 Hz at a 150 Hz crossover is attenuated about 38.3 dB. DC block
also affects very low audio. +6 dB is approximately double; exact doubling
is +6.0206 dB. Metering uses sample peaks, not true peaks, every 50 ms.
