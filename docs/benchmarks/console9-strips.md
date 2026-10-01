# Console 9 engine strip cost

2026-09-30, Intel Core Ultra 5 225F, Clang release preset, stereo 48 kHz,
512-frame blocks, engine revision `1dc7a67`. The existing B1 tool now accepts `--strips`:

```sh
cmake --preset release
cmake --build --preset release --target engine-bench -j6
build/release/host/engine/engine-bench --strips
```

Each row times 2,097,152 frames after warm-up, processing one engine strip
through `processNode`. The channel hears a constant 0.01 device stand-in;
the buss receives an already encoded signal. The cost includes taps, control
handling and, for the buss, summation and the neutral decoder. No inserts,
media, worker dispatch, audio device or hardware deadline measurement.

| Strip | Console 9 | ns/stereo frame | One 48 kHz strip, % of one core |
|---|---|---:|---:|
| Channel | linear | 8.63 | 0.041 |
| Buss | linear | 12.83 | 0.062 |
| Channel | on | 37.96 | 0.182 |
| Buss | on | 70.35 | 0.338 |

These are local throughput measurements, not timing acceptance thresholds.
A non-sharing audio send adds its own Channel encoder; a unity post-fader
send with pan `follow` shares the strip output.

The engine level tests measured a unity channel/master gain of −6.518658 dB,
an on/off difference of 2.91e-11 at 0.001 input, and a silence peak of
−152.423 dBFS over 2,048 samples with project seed 12345. Two correlated
0.5 inputs through a neutral buss measured +0.604133 dB relative to linear
mode: the inverse Buss curve expands coincident signals before clipping.
