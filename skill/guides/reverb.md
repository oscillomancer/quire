# Reverb: Nave

Nave (`com.suppleaudio.reverb`) supplies early reflections and a diffuse,
stereo late tail. Use it on an auxiliary return at `mix: 100`, or insert it
with a modest mix. It accepts flat physical-unit merge patches with values under `params`, schema
`supple.reverb-patch/1.0`; query `fx.describe` for the instance's keys.

| Key | Unit and range | Default |
|---|---|---:|
| `size` | %, 0–100; geometric line-length scale | 50 |
| `decay` | ms, 100–60000; mid-band RT60 | 2000 |
| `predelay` | ms, 0–500 | 10 |
| `low_decay` | ratio, 0.25–4; low RT60 multiplier | 1 |
| `high_decay` | ratio, 0.1–1; high RT60 multiplier | 0.5 |
| `crossover` | Hz, 100–1000 | 250 |
| `damping_freq` | Hz, 1000–16000 | 6000 |
| `diffusion` | %, 0–100 | 70 |
| `early` | dB, −60 (exactly off) to +6 | −6 |
| `modulation` | %, 0–100; up to ±0.5 ms | 20 |
| `width` | %, 0–200; wet side gain | 100 |
| `low_cut` | Hz, 20–1000; input highpass | 20 |
| `high_cut` | Hz, 1000–20000; input lowpass | 16000 |
| `freeze` | bool; hold the late network, reject new wet input | false |
| `mix` | %, 0–100; linear dry/wet blend | 20 |
| `output` | dB, −24 to +24 | 0 |

Try these three starting patches, then render and compare on a branch:

- **Short drum room:** `{"params":{"size":25,"decay":650,"predelay":4,"early":-3,"diffusion":85,"low_cut":120,"high_cut":10000,"mix":12}}`. Keep onset definition and avoid low-frequency buildup.
- **Vocal hall:** `{"params":{"size":85,"decay":4500,"predelay":25,"high_decay":0.4,"low_cut":180,"mix":18}}`. Predelay separates the voice from its tail; use `mix:100` on a send return.
- **Held texture:** first excite the return, then automate `freeze:true`; set `width:0` to check the mono result. Dry audio still passes according to mix. Unfreeze to restore decay; held size changes take effect afterwards.

The editor plots the decay design target and input-bandwidth curve, with
sample-peak input/output meters. Input cuts change excitation, not loop RT60.
Size/predelay changes glide and bend pitch. Frequencies clamp internally to
0.45 times the rate without changing patch values. The static octave RT60
measurements use modulation off; a separate long bright-tail check covers
default modulation. FDN reads use a 12-point interpolator to reduce the
extra high-frequency loss caused by cubic reads. Extreme shelf multipliers and the
100 ms / maximum-size combination are outside the measured RT60 matrix.
Diffuser gain is capped for very short tails so its own ringing cannot outlast
the requested decay. See `docs/specs/stock-effects.md` §7 for measurements.

Nave has zero reported processing latency; predelay and the audible onset
remain musical delays. Its CLAP tail is infinite while frozen; otherwise it
is predelay + 1.5 times the longest band RT60 + 100 ms. Wet width above 100%
can raise peaks and reduce mono compatibility. There is no automatic limiter
or loudness compensation: adjust send/output level and re-measure the mix.
