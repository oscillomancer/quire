# Compressor and limiter (`comp`)

Ballast controls track dynamics, ducks a track from an external key, or
limits a master to a peak ceiling. CLAP id: `com.suppleaudio.comp`.
Patches use `supple.comp-patch/1.0` with physical values under `params`.

| Key | Unit / range | Default |
|---|---|---|
| `mode` | `compressor`, `limiter` | `compressor` |
| `threshold` | dB, -60..0 | -18 |
| `ratio` | ratio, 1..100; 100 = infinity, pow 3 curve | 4 |
| `knee` | dB, 0..24, full width | 6 |
| `attack` | ms, 0.01..300, log | 10 |
| `release` | ms, 5..5000, log | 150 |
| `auto_release` | bool | false |
| `detector` | `peak`, `rms` | `peak` |
| `rms_window` | ms, 1..100, log; exponential averaging | 10 |
| `topology` | `feedforward`, `feedback` | `feedforward` |
| `sidechain` | `internal`, `external` | `internal` |
| `sc_highpass` | Hz, 10..500, log; 10 = off | 10 |
| `sc_lowpass` | Hz, 1000..20000, log; 20000 = off | 20000 |
| `sc_listen` | bool, filtered-key audition | false |
| `stereo_link` | percent, 0..100 | 100 |
| `lookahead` | `off`, `ms1`, `ms5`; host restart | `off` |
| `makeup` | dB, -12..24 | 0 |
| `auto_makeup` | bool | false |
| `ceiling` | dB, -24..0; limiter ceiling | -1 |
| `true_peak` | bool; host restart when latency changes | true |
| `mix` | percent, 0..100; delayed dry/wet | 100 |
| `output` | dB, -24..24 | 0 |
| `gr` | dB, -60..0, readonly, deepest since report | 0 |

## Three starting recipes

Track control: merge `{"params":{"threshold":-24,"ratio":3,"knee":6,
"attack":20,"release":150,"auto_makeup":true}}`. Render, compare crest
factor and loudness, and set makeup for a fair A/B. Auto makeup compensates
static reduction at -18 dBFS, so it is a starting estimate rather than a
measured loudness match.

Kick-keyed bass: merge `{"params":{"sidechain":"external","threshold":-24,
"ratio":10,"knee":0,"attack":2,"release":120}}`. Connect the kick using
`route.add {"from":"tr_kick","fx":"fx_bass_comp","tap":"post_fader"}`.
Listen to the key with `sc_listen`, then turn audition off. Missing or silent
keys yield no compressor reduction; key filters can remove unwanted triggers.

Master peak delivery: merge `{"params":{"mode":"limiter","lookahead":"ms1",
"true_peak":true,"ceiling":-1,"output":3}}`. Render and check
`loudness.true_peak_dbtp` and integrated LUFS. Adjust output drive only if
more limiting is wanted. This controls peaks; it does not normalize LUFS.

## Limits and timing

The limiter ignores compressor threshold/ratio/topology/detector and external
key for gain control. Ceiling applies after makeup, mix and output, including
key audition. Its true-peak reconstruction guard is conservative and can
reduce loud signals more than their input peak alone suggests. Below-ceiling
transparency is verified for a -10 dBFS sine with a -1 dB ceiling.

Lookahead delays dry and wet equally. With limiter true-peak detection, add
16 samples to the selected lookahead. `off` guarantees sample peaks only,
even if true-peak is selected. Mode, lookahead and true-peak are stepped,
non-automatable controls; changes wait for host restart, including a mode
switch with equal latency.
Feedback detects the previous compressed wet sample before makeup/mix/output;
external key overrides feedback. Auto release blends release and five-times
release coefficients according to reduction held over the last second.
The GUI peak readouts are sample peak dBFS, and `gr` is not a loudness meter.
