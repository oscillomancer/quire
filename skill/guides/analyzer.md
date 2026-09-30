# Live analyzer

The stock analyzer (`com.suppleaudio.analyzer`, working display name Gauge)
monitors any point in a signal chain without altering a sample. Put it after
the processing you want to inspect. It supports mono/stereo and float/double,
with zero latency. It does not limit, normalize or correct the sound.

| Key | Unit/type | Range | Default |
|---|---|---|---|
| `window` | enum | `s3`, `s10`, `s30`, `since_reset` | `s3` |
| `reset` | momentary bool | false/true | false |
| `hold` | bool | false/true | false |
| `integrated` | LUFS, readonly | −120..40 | −120 |
| `short_term` | LUFS, readonly | −120..40 | −120 |
| `momentary` | LUFS, readonly | −120..40 | −120 |
| `lra` | LU, readonly | 0..120 | 0 |
| `true_peak` | dBTP, readonly | −120..40 | −120 |
| `correlation` | unitless, readonly | −1..1 | 1 |
| `width` | percent, readonly | 0..100 | 0 |
| `balance` | dB, readonly | −120..120 | 0 |
| `centroid` | Hz, readonly | 0..20000 | 0 |

Use physical values in semantic merge patches, for example:
`{"params":{"window":"s10","hold":false}}`. Writing `reset: true` clears
measurements and returns the control to false. A window change starts a fresh
measurement history. Hold freezes accumulation; incoming audio is excluded
until hold is released. Reset works while held. These controls never change
audio. Meter values are excluded from saved patches.

Three recipes:

1. **Check delivery loudness.** Select `since_reset`, trigger reset before the
   programme, and read integrated LUFS, LRA and true peak after playback.
   Compare against the delivery target; make changes elsewhere and repeat.
2. **Compare verses and choruses.** Use `s10` and watch short-term versus
   momentary LUFS. Use hold to inspect the current reading. Release hold and
   reset before measuring another passage.
3. **Inspect stereo compatibility.** Put the analyzer after stereo processing.
   Correlation near −1 indicates opposing channels; width 0 is purely mid,
   width 100 is purely side. Positive balance means right is louder. The
   spectrum and centroid measure `(L+R)/2`, so anti-phase material cancels
   there even though the channels and true-peak meter remain active.

The optional semantic extension member `get_telemetry` writes compact
`supple.analysis-report/1.0` JSON with source, loudness, spectral and stereo
fields, plus live window/hold/drop metadata. A host must check `struct_size`
and the callback pointer. Existing DAW commands do not yet expose that new
callback directly; generic hosts can read the readonly scalar parameters.
The JSON spectrum contains 31 values, 20 Hz to 20 kHz, dBFS-scaled so a
full-scale sine is 0 dB in its band. It is smoothed in power over 300 ms;
it is not an offline LTAS or a per-band stereo-width measurement.

Finite horizons expire every 100 ms; integrated loudness uses complete 400 ms
blocks inside the horizon, LRA uses complete 3 s blocks. M/S need 400 ms/3 s
of unheld audio. Unavailable/silent level meters use −120; LRA defaults to 0.
Loudness histograms approximate LRA to 0.01 LU. At higher sample rates true
peak uses core-analysis's existing 2×/1× policy. Measurement packets can lag
by a few milliseconds; under overload, new packets are dropped, the count
is reported and history restarts rather than silently joining discontinuous
audio. Audio is always intact. Use offline render analysis for final delivery
validation, gating-sensitive short passages and reproducible full reports.
