# Facet (EQ): verification

The filter designs are original: minimum phase uses TPT state-variable
filters and Butterworth cascades, natural phase uses magnitude-matched
second-order sections at the host rate (double-precision DF2T state, a fixed
16-sample coefficient grid while smoothing; bells and shelves add a small
fitted correction section), and linear phase uses partitioned FIR
convolution. Details and limits: `QUIRE.md` and `docs/specs/stock-effects.md`
§1.

## Accuracy

The accuracy target is set for bells with Q ≤ 4 and shelves with Q ≤ 1,
f0 ≤ 10 kHz, measured to 20 kHz at 44.1 and 48 kHz over the full ±30 dB gain
range. Larger Q and near-Nyquist cases are measured limits, not guarantees.

Method: 25 f0 values, gain in 3 dB steps, Q 0.1–4 for bells and 0.1–1 for
normal shelves, 2049 logarithmic and 2049 linear frequency probes plus
resonance probes per case (`eq-bench --accuracy`). The shelf design is
reciprocal, so both directions have the same worst error.

| Domain | 44.1 kHz worst | 48 kHz worst |
|---|---:|---:|
| Normal bells | 0.174 dB | 0.060 dB |
| Normal shelves | 0.254 dB | 0.223 dB |
| Resonant shelves, f0 ≤ 10 kHz, Q 1.2–24 (limit) | 17.2 dB | 0.84 dB |
| Bells near Nyquist, Q 4–24 (limit) | 1.50 dB | 1.29 dB |
| Shelves near Nyquist, Q 1.2–24 (limit) | 21.0 dB | 16.2 dB |

The resonant-shelf fit tries the analog bump, rejects unrealizable or
opposite-extremum fits, and keeps damped analog zeros without aliasing
out-of-band zeros.

## CPU

`eq-bench 120` (release, `-O2`), median of five CPU-time trials per fixed
non-flat patch, 120 audio seconds each, 48 kHz stereo, 512-frame blocks:

| Bands | Auto gain | Minimum (% of a core) | Natural (% of a core) |
|---:|---|---:|---:|
| 4 | off | 0.31 | 0.24 |
| 8 | off | 0.61 | 0.43 |
| 4 | on | 0.34 | 0.31 |
| 8 | on | 0.67 | 0.50 |

No benchmark runs as a timing test; patch definitions and loops are in
`engine/bench/eq_bench.cpp`.

## What the tests cover

Normal-domain accuracy, analog/processed agreement, matched Butterworth
cutoffs, block-partition invariance, automation and silent tails in every
phase mode, canonical patch and state, and real-time host contracts.

Editor previews: `build/dev/plugins/eq/eq-gui-preview 0 out.ppm 1 flat` (or
`mix`). On Linux the preview embeds in a fixed-size parent so a tiling window
manager cannot change the capture size.
