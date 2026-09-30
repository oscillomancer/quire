# Facet: parametric EQ

Use `com.suppleaudio.eq` for rumble removal, tonal shaping and cuts around regions
reported by `analyze.masking`. Patches use `supple.eq-patch/1.0`, with physical
values under `params`. A merge changes only the specified keys.

Each `bandN` below means all eight bands, N = 1…8. IDs are permanent; address
keys rather than guessing host-normalized values.

| Key | Unit and range | Default |
|---|---|---|
| `bandN.enabled` | bool | bands 1–4 on; 5–8 off |
| `bandN.type` | bell, low_shelf, high_shelf, high_pass, low_pass, notch, band_pass, tilt | high_pass, bell, bell, high_shelf; then bell |
| `bandN.freq` | Hz, 10…30000, logarithmic | 30, 200, 1000, 8000, 500, 2000, 4000, 12000 |
| `bandN.gain` | dB, −30…+30 | 0 |
| `bandN.q` | unitless, 0.1…24, logarithmic | 0.71 |
| `bandN.slope` | db6, db12, db18, db24, db36, db48, db72, db96 (dB/oct labels) | "db24" |
| `bandN.channel` | stereo, left, right, mid, side | stereo |
| `phase` | minimum, natural, linear; nonautomatable, requires restart | natural |
| `auto_gain` | bool | false |
| `output` | dB, −24…+24 | 0 |

Bands are serial in index order. Gain applies to bells, shelves and tilt; Q is
width/resonance, with the user Q used by 12 dB/oct passes and fixed Butterworth
Qs for higher slopes. Tilt applies −gain/2 to lows and +gain/2 to highs. A side
band leaves the mono sum untouched; a mid band changes only the mono component.

Remove rumble:

```json
{"params":{"band1.enabled":true,"band1.type":"high_pass","band1.freq":"80 Hz","band1.slope":"db24"}}
```

Carve a masking region: after measuring a conflict around 250 Hz, cut the less
important source, then render and compare again:

```json
{"params":{"band2.enabled":true,"band2.type":"bell","band2.freq":250,"band2.gain":-3,"band2.q":1.2}}
```

De-harsh the centre while preserving the sides:

```json
{"params":{"band3.enabled":true,"band3.type":"bell","band3.freq":3500,"band3.gain":-2.5,"band3.q":1.4,"band3.channel":"mid"}}
```

Minimum phase is zero reported latency and uses a bilinear design. Natural uses
16×…128× IIR oversampling to approximate analog magnitude near Nyquist; it also has
zero reported latency (its phase and group delay are frequency-dependent).
Disabled/zero-gain bands are exact bypasses. The factory patch has a 30 Hz
high-pass, so it intentionally removes subsonic audio; disable band1 for a flat
null comparison. Internal frequency clamps to 0.49 × the sample rate, while the
patch retains the requested Hz value.

Linear phase reports round(2048 × sampleRate / 48000) samples. Its symmetric,
finite FIR can ring before transients and cannot represent arbitrarily narrow
low-frequency filters exactly. Linear-band changes compile asynchronously and
coalesce, then crossfade over 20 ms; use minimum/natural for dense sample-accurate
automation. Phase changes require host reactivation. Auto gain compensates the
mean static curve on a log-frequency axis, capped at ±24 dB compensation; it is not a loudness measurement and
is only an estimate for selective channel placement. The live LTAS is displayed
while the editor is open; it is not an audio-path loudness meter. Gains accumulate:
this EQ has no ceiling limiter. Check true peak after large boosts.
