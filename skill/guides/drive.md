# Kiln [drive]: saturation

Use `com.suppleaudio.drive` for harmonic colour, asymmetric distortion and
wavefolding. Patches use `supple.drive-patch/1.0`; merge changes under `params`.
Render and compare loudness before deciding that added brightness is an improvement.

| Key | Unit / choices | Range | Default |
|---|---|---|---|
| `curve` | `tape`, `tube`, `transistor`, `diode`, `fold`, `hard` | enum | `tape` |
| `drive` | dB | 0–36 | 6 |
| `bias` | percent | −100–100 | 0 |
| `tone` | dB/oct control (canonical dB) | −6–6 | 0 |
| `oversampling` | `x1`, `x2`, `x4`, `x8` (display 1x/2x/4x/8x) | enum; restart required | `x4` |
| `auto_gain` | bool | false/true | true |
| `mix` | percent | 0–100 | 100 |
| `output` | dB | −24–24 | 0 |
| `input_level`, `output_level` | readonly dBFS sample peaks | −120–40 | −120 |

Drive precedes the static curve. Bias adds up to ±1 before shaping, then a 5 Hz
DC blocker removes the offset. Tone is a first-order tilt centred on 1 kHz:
DC/HF gains are −2·tone/+2·tone dB, with its exact inverse after the curve.
The slope varies with frequency; this is not a broadband straight-line EQ.
Positive tone drives high frequencies harder. Floats ramp over 20 ms; curve and
auto-gain changes crossfade over 10 ms. Oversampling changes require reactivation;
the current sound and latency remain until the host performs the restart.
Latency: 1, 513, 577, 609 samples at 1x/2x/4x/8x, at every sample rate. Dry mix
uses the same integer delay. 1x has intentional ADAA high-frequency attenuation.

Three starting recipes (semantic merge patches):

- Gentle colour: `{"params":{"curve":"tape","drive":6,"bias":0,"tone":0,"oversampling":"x4","auto_gain":true,"mix":100,"output":0}}`.
- Asymmetric tube: `{"params":{"curve":"tube","drive":12,"bias":30,"tone":1,"oversampling":"x4","auto_gain":true,"mix":60,"output":-1}}`.
- Folded parallel texture: `{"params":{"curve":"fold","drive":24,"bias":30,"tone":0,"oversampling":"x8","auto_gain":true,"mix":25,"output":-2}}`.

These are original static shapers, not physical tape/circuit simulations. Tape
has a wide soft knee; tube starts gently and compresses more at large signals;
transistor has a flatter low-level knee; diode is tanh's exponential knee.
Fold combines a sine folder with a soft residual path and is naturally
non-monotonic: more drive need not mean more THD. Hard is clipping at ±1.

Auto gain compensates the *RMS* gain of Gaussian noise at −18 dBFS RMS, using a
prepare-time table per curve at 0.25 dB drive steps and linear interpolation.
It is not a programme loudness rider and does not compensate bias or tone.
On deterministic 20 Hz–20 kHz pink noise at −18 dBFS RMS, drives 0/6/12/18/24/30/36,
4x, bias/tone zero, the largest absolute LUFS difference was tape 0.328, tube
0.333, transistor 0.326, diode 0.350, fold 0.913, hard 0.357 LU. At −30 dBFS RMS,
differences across drive ranged approximately −0.12 to +13.90 LU; at −6 dBFS RMS,
−14.44 to 0 LU. Set output by measured LUFS for material away from the reference.

Aliasing measurement: 5 kHz sine, **−18 dBFS RMS** (−14.9897 dBFS peak), drive
24 dB, 48 kHz, bias/tone zero, auto gain off; coherent FFT after 1 s settling.
Worst non-harmonic bins relative to the fundamental at 4x: tape −155.26,
tube −104.89, transistor −134.73, diode −153.48 dBc. Fold: −154.67/−154.50 dBc
at 4x/8x; hard: −81.42/−91.92 dBc. These figures describe that input level,
not a universal alias bound. **Prefer 8x for hard/fold**, particularly when
hitting them harder or adding tone/bias. Output can exceed 0 dBFS; Kiln is not
a mastering limiter. Compare renders at matched loudness and check peaks.
