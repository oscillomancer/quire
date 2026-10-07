# Nave (reverb): verification

Nave is a 16-line Hadamard feedback delay network with seeded early
reflections, input diffusion, three-band decay and seeded modulation. The
design choices (second-order Butterworth-Q shelves, the diffuser-tail cap,
twelve-point Lagrange reads in the FDN) are specified in
`docs/specs/stock-effects.md` §7.

## RT60

RT60 is measured from octave-filtered stereo impulse energies with Schroeder
integration and T30 regression (−5 to −35 dB), never from timing. The
matrix has 90 responses: 44.1, 48, 88.2, 96 and 192 kHz; decay 0.5, 2 and 8 s;
size 20, 50 and 90; two shapes (defaults, and low_decay 1.5 with a 1 kHz
crossover and 2 kHz damping), with high_decay 0.5, modulation 0, early
reflections off and mix 100 %. Only octave bands at least an octave from
both transitions are compared with the plateau targets.

| Band | Worst relative RT60 error |
|---:|---:|
| 125 Hz | 7.44 % |
| 250 Hz | 4.35 % |
| 500 Hz | 7.48 % |
| 1 kHz | 6.03 % |
| 2 kHz | 7.68 % |
| 4 kHz | 6.32 % |
| 8 kHz | 2.70 % |

With default modulation (size 20, decay 8 s, damping 2 kHz), the 8 kHz RT60
is 3.946 s at 44.1 kHz and 3.949 s at 48 kHz against 4 s (1.35 % and 1.27 %).
Extreme ratios and overlapping transitions are outside this matrix.

## Density, width, freeze

Normalized echo density reaches 0.8 at 67 ms by default and at 52 ms with
diffusion 100 (20 ms window, crossing sustained for 10 ms). Late-tail
correlation is 0.049 at width 100; width 0 renders identical channels.
Freeze changes a ten-second stereo tail's energy by −0.03 dB.

## CPU

Release build (`-O2`), 48 kHz stereo, 512-frame blocks, editor closed,
median of five CPU-time trials of 120 audio seconds (`reverb-bench 120`):
default 1.51 %, hall 1.66 %, maximum texture 1.84 % of one core. Maximum
texture is size 100, decay 60 s, low_decay 4, high_decay 0.1, diffusion and
modulation 100, width 200, mix 100.

## Tests

The routine suite (`reverb-engine.tests`, in every preset) uses
representative short impulse responses; its RT60 subset's worst error is
8.04 %, and it keeps the ten-second freeze comparison and a five-second
automation stress test. The full matrix above is the opt-in
`reverb-engine.measure` target, built from the same source and not
registered with CTest:

```sh
cmake --build --preset release --target reverb-engine.measure
build/release/plugins/reverb/engine/reverb-engine.measure
```

The plugin suite covers concurrent semantic edits and audio-thread tail
notifications; clap-validator passes; the built CLAP exports only
`clap_entry`. Editor previews: `reverb-gui-preview 0 out.ppm 1 default` (or
`hall`); `reverb-linux-host-probe` checks X11 embedding, resize, show/hide,
three reopen cycles and timer teardown.
