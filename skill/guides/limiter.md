# Treeline: limiter

Use `com.suppleaudio.treeline` last on the master (or on a bus) to make a
mix louder without letting any peak past a ceiling, and to measure its
loudness while you do. Patches use `supple.treeline-patch/1.0`, with
physical values under `params`; a merge changes only the keys it names.

It has three controls because the rest adapts to the music by itself:

- **True peak.** The ceiling holds between samples too: the detector reads
  the signal at three points between each pair of samples, within 0.01 dB
  to 20 kHz.
- **Attack.** Every reduction arrives along a smooth curve that has landed
  when its peak plays (4.5 ms of latency at 48 kHz). Hits get a sharp 1 ms
  curve, which keeps their snap; held sounds a broad 4 ms one, which keeps
  them clean.
- **Bass guard.** Under a strong bass line each reduction is held for a
  whole cycle of the bass, so the gain never follows the waveform (the
  low-end distortion most limiters add).
- **Release.** Peaks above what the music sustains recover at `release`; the
  sustained part over about ten times longer (0.4 to 4 s), so dense passages
  do not pump and a lone hit does not dent what follows.

| Key | Unit and range | Default |
|---|---|---|
| `gain` | dB, 0…24: drive into the ceiling | 0 |
| `ceiling` | dB true peak, −24…0: the highest the output goes | −1 |
| `release` | ms, 10…1000, logarithmic: how fast peaks recover | 120 |
| `target` | LUFS, −24…−6: the integrated loudness the meters read against | −14 |
| `listen` | output, matched, delta: the output; the output at the input's loudness; what it removes. For people, not mixes | output |
| `gain_reduction` | dB, readonly: the deepest reduction in the last block | |
| `short_term_loudness` | LUFS, readonly: the output over the last 3 s | |
| `integrated_loudness` | LUFS, readonly: the output since the transport started | |
| `true_peak` | dBTP, readonly: the output's highest true peak since the transport started | |
| `loudness_range` | LU, readonly: EBU R128 LRA since the transport started | |

The meters follow BS.1770 and EBU R128 (K-weighting, 400 ms blocks gated
at −70 LUFS and 10 LU under, short-term 3 s, LRA from the 10th to the 95th
percentile). They restart when the transport starts.

## Recipes

A mix 3 dB louder with a −1 dBTP ceiling (common streaming delivery):

```json
{"params":{"gain":3,"ceiling":-1}}
```

Aim for a loudness: set `target`, play the whole song, read
`integrated_loudness`, and change `gain` by the difference; above the
ceiling loudness rises less than the gain, so play it again and repeat once.

```json
{"params":{"target":-14}}
```

Broadcast (EBU R128: −23 LUFS, −1 dBTP or lower), slow and safe:

```json
{"params":{"gain":2,"ceiling":-2,"release":200,"target":-23}}
```

Dense club master, faster recovery:

```json
{"params":{"gain":9,"ceiling":-0.3,"release":60,"target":-8}}
```

## Measuring it

`gain_reduction` that stays below 2 to 3 dB on the loudest passages is
usually transparent; much more trades punch for loudness. Peak to loudness
(the editor shows it: true peak minus integrated) falls as it works harder.
Compare by ear with `listen` `matched`: the output turned down to the
input's loudness, so what is left to hear is what limiting costs, not the
gain. Render and `compare` at matched loudness for the same reason.

## Limits

There is one stereo-linked gain, so the image never moves; no mid/side or
per-band limiting. The latency is fixed (4.5 ms at 48 kHz); nothing
restarts it. The ceiling is measured with Treeline's own true-peak reading
(flat to 21 kHz at 48 kHz). Another meter's interpolator may read a
little differently: BS.1770's example filter has 0.17 dB of ripple of its
own, and clipped material with strong content above 21 kHz can read up to
half a dB higher on a meter that reconstructs to 24 kHz. The loudness meters measure the output while it plays;
for a whole-file figure, render and `analyze`. Render normalization still
has its own true-peak limiter for delivery files.
