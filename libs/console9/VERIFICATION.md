# Console 9: verification

How the port relates to Airwindows' code, and why block sizes above one can
differ in the last bits, is explained in `README.md`.

## Null test against the reference

`console9.tests` compares the port with the vendored reference
(`third_party/airwindows-console9`, test-only). Each row covers 2,580,480
samples from 315 native fader, pan and sample-rate combinations, with sine
sweeps and noise from −60 to +12 dBFS, silence, and inputs on both sides of
the float/double 1.18e−23 threshold. At block size 1 the output is
bit-identical. At every block size, all of the reference's block-endpoint
samples are identical, and supplying the reference's per-block interpolated
coefficients to the port reproduces every reference sample bit for bit.

| Stage | Bits | Block | Identical % | Max float ulps, all | Max absolute error | Ordinary max float ulps |
|---|---:|---:|---:|---:|---:|---:|
| channel | 32 | 1 | 100.000000 | 0 | 0 | 0 |
| channel | 32 | 7 | 94.559152 | 3.87688e+37 | 5.44679e-08 | 0 |
| channel | 32 | 64 | 99.702381 | 2.85626e+25 | 5.44546e-08 | 0 |
| channel | 32 | 512 | 98.511905 | 3.87842e+37 | 5.44639e-08 | 0 |
| channel | 32 | 4096 | 98.549107 | 3.8504e+37 | 5.44639e-08 | 0 |
| channel | 64 | 1 | 100.000000 | 0 | 0 | 0 |
| channel | 64 | 7 | 89.192437 | 1.67772e+07 | 2.6739e-15 | 1.49012e-08 |
| channel | 64 | 64 | 99.212123 | 1.67765e+07 | 2.26348e-15 | 1.67638e-08 |
| channel | 64 | 512 | 96.800014 | 1.67768e+07 | 2.41525e-15 | 1.30385e-08 |
| channel | 64 | 4096 | 96.888060 | 1.67772e+07 | 2.08449e-15 | 1.30385e-08 |
| buss | 32 | 1 | 100.000000 | 0 | 0 | 0 |
| buss | 32 | 7 | 94.559152 | 3.87688e+37 | 5.44679e-08 | 0 |
| buss | 32 | 64 | 99.702381 | 1.42813e+25 | 5.44546e-08 | 0 |
| buss | 32 | 512 | 98.511905 | 3.87842e+37 | 5.44639e-08 | 0 |
| buss | 32 | 4096 | 98.549107 | 3.8504e+37 | 5.44639e-08 | 0 |
| buss | 64 | 1 | 100.000000 | 0 | 0 | 0 |
| buss | 64 | 7 | 87.808625 | 1.67768e+07 | 1.77636e-15 | 9.31323e-09 |
| buss | 64 | 64 | 99.142485 | 1.67762e+07 | 1.77636e-15 | 1.11759e-08 |
| buss | 64 | 512 | 96.352694 | 1.67768e+07 | 1.77636e-15 | 1.11759e-08 |
| buss | 64 | 4096 | 96.483212 | 1.67768e+07 | 1.77636e-15 | 1.30385e-08 |

"Ordinary" excludes only the hard-right left output (A = 1). At that edge
the reference's `a*t+a*(1-t)` produces a tiny ±pan gain instead of exact
zero, float dither then sees a different exponent, and the difference can
reach about 5.45e−8; relative ulps against a near-zero reference value are
correspondingly large. Elsewhere double errors stay at rounding scale. These
differences come from the port's partition-independent settings and ramps,
not from changed curve or dither arithmetic. Mono is checked against the
equivalent fader-only stereo hard-left path.

Five encoded channels summed and decoded by a neutral buss:

| Bits | Block | Identical % | Max float ulps | Max absolute error |
|---|---:|---:|---:|---:|
| 32 | 1 | 100.000000 | 0 | 0 |
| 64 | 1 | 100.000000 | 0 | 0 |
| 32 | 7 | 100.000000 | 0 | 0 |
| 64 | 7 | 68.383789 | 1.6652e-06 | 1.11022e-16 |
| 32 | 64 | 100.000000 | 0 | 0 |
| 64 | 64 | 96.301270 | 8.32602e-07 | 5.55112e-17 |
| 32 | 512 | 100.000000 | 0 | 0 |
| 64 | 512 | 90.966797 | 1.06171e-07 | 8.32667e-17 |
| 32 | 4096 | 100.000000 | 0 | 0 |
| 64 | 4096 | 90.893555 | 4.17233e-07 | 5.55112e-17 |

The coefficient-injection diagnostic reproduces every encoder and decoder
difference in the sum test exactly. Ramp split tests compare sample bits,
signed zero included, for blocks 1, 7, 64, 512 and 4096 in all four stage
types and both precisions, retargeting included.

A build with tests disabled contains neither the reference target nor the
upstream files.

## Linear mode ("Console 9 off")

`setLinear(true)` keeps the fader gain, pan law and ramps and skips only the
curves, denormal replacement and dither (ADR-0019 §6). The Channel also
skips the ×0.618 pre-scale (its product with the encoder's small-signal
slope is unity); the Buss uses the exact slope 0.6180339887498949 before pan
and fader. There is no RNG advance, noise, clipping or internal crossfade;
a plan rebuild handles the on/off transition. Nonfinite input is treated as
zero. All four stages support the mode, and it is off by default.

Tests check exact neutral cascade gain for signed powers of two from 2^−40 to
2^20 with no clipping; small signals against the curves within two float ulps
(dither) or 16 double ulps (rounding); bit-exact split ramps; silent output
for silent input and preservation of tiny inputs; nonfinite input; and that
the RNG does not advance in linear mode (the next curves-on sample after 1000
linear samples matches a control). All cover stereo and mono and both I/O
precisions.

## CPU

`console9-bench`, Clang release build, 512-frame blocks × 4096 iterations
after 32 warm-up blocks:

| Stage | Bits | Mode | Settings | ns per stereo frame | One core at 48 kHz |
|---|---:|---|---|---:|---:|
| channel | 32 | curves | constant | 32.657 | 0.1568 % |
| channel | 32 | curves | ramping | 37.458 | 0.1798 % |
| buss | 32 | curves | constant | 25.417 | 0.1220 % |
| buss | 32 | curves | ramping | 35.349 | 0.1697 % |
| channel | 64 | curves | constant | 14.105 | 0.0677 % |
| channel | 64 | curves | ramping | 23.710 | 0.1138 % |
| buss | 64 | curves | constant | 13.157 | 0.0632 % |
| buss | 64 | curves | ramping | 20.727 | 0.0995 % |
| channel | 32 | linear | constant | 1.465 | 0.0070 % |
| channel | 32 | linear | ramping | 7.076 | 0.0340 % |
| buss | 32 | linear | constant | 1.525 | 0.0073 % |
| buss | 32 | linear | ramping | 7.332 | 0.0352 % |
| channel | 64 | linear | constant | 0.652 | 0.0031 % |
| channel | 64 | linear | ramping | 6.783 | 0.0326 % |
| buss | 64 | linear | constant | 0.945 | 0.0045 % |
| buss | 64 | linear | ramping | 6.905 | 0.0331 % |
