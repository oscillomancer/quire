# Lichen: measuring a point in the chain

Use `com.suppleaudio.lichen` to measure audio anywhere in a chain: after a
track's processing, on a bus, on the master. It never changes a sample (its
output is its input, bit for bit), adds no latency and reports everything as
read-only parameters in physical units. Put it after what you want to
measure; read it with `fx.meters`.

## Reading it

| Where | How |
|---|---|
| DAW, after a render | `render.mix`, then `fx.meters {"fx": "fx_3", "from": "render"}`: the readings as that render ended. Offline renders use plugin instances of their own, so this is the way to measure without playing in real time |
| DAW, while playing or monitoring | `fx.meters {"fx": "fx_3"}` (`from` defaults to `live`): the playing instance as it stands. `source` says what you got: `live`, `render`, or `none` (nothing has run yet) |
| Sandbox | `dsp.test` with `plugin: "com.suppleaudio.lichen"` reports them as `meters`; or `plugin.load`, `render`, then `param.get` |

The readings come in two kinds:

- **Now:** `momentary` (400 ms) and `short_term` (3 s) loudness.
- **The play:** everything else, from when play starts until it stops, after
  which they hold. A render plays, so it covers exactly the render. A play
  starts them afresh unless `restart_on_play` is false; any change to
  `reset` starts them afresh too. With the transport never started (a live
  input, `dsp.test`) they gather until reset. `measured` says how much audio
  they cover: check it before trusting them.

## Readings

| Key | Unit | Meaning |
|---|---|---|
| `integrated` | LUFS | BS.1770 gated loudness of the play (−70: nothing measured) |
| `short_term`, `momentary` | LUFS | 3 s and 400 ms loudness, now |
| `short_term_max`, `momentary_max` | LUFS | The loudest 3 s and 400 ms of the play |
| `lra` | LU | EBU R128 loudness range: under 5 compressed and steady, 5–10 typical pop, above 15 dynamic |
| `true_peak` | dBTP | Highest inter-sample peak of the play |
| `sample_peak` | dBFS | Highest sample |
| `plr` | dB | True peak less integrated: under 8 very limited, 9–14 typical, above 14 open |
| `crest` | dB | Sample peak against RMS |
| `overs` | count | Times the true peak went above 0 dBTP |
| `clipped` | count | Runs of three or more samples held at full scale: clipping already in the audio |
| `dc_offset` | dBFS | The larger channel's mean; above −60 is worth a high-pass |
| `correlation` | −1..1 | +1 mono, 0 unrelated, below 0 the channels oppose |
| `width` | % | Side's share of the energy: 0 mono, 50 unrelated, 100 opposed |
| `low_width` | % | The same below 250 Hz; keep it near 0 (under 25) for a solid low end |
| `balance` | dB | Right against left |
| `mono_loss` | LU | How much quieter the mono fold-down is: 0 mono, about 3 unrelated, more means cancellation |
| `centroid` | Hz | Median spectral centre of mass: higher sounds brighter |
| `tilt` | dB/oct | Slope 100 Hz–10 kHz: pink noise 0, most mixes −1 to −3; more negative sounds darker |
| `sub`, `low`, `low_mid`, `mid`, `high_mid`, `presence`, `air` | dB | Each band's share of the whole: 20–60, 60–250, 250–500, 500–2k, 2k–4k, 4k–6k, 6k–20k Hz |
| `key` | key | `a_minor`, `e_flat_major`, … or `none`: the key the notes heard most resemble |
| `key_strength` | % | How well they match it: above 70 clear, below 50 doubtful |
| `measured` | ms | Audio the play's readings cover |

The tone readings (bands, tilt, centroid), the stereo readings and the
loudness use the definitions of `analyze`, so a live reading compares with
an analysis report of the same audio and with `compare` against a
reference.

Settings (none change the sound): `target` (LUFS, −30…−5, default −14: the
editor's reference), `restart_on_play` (default true), `reset` (any change
restarts the readings), `slope` (`flat`, `db3`, `db4_5`: how the editor
tilts its spectrum). Factory patches set the target: Streaming (−14),
Streaming, Quieter (−16), Broadcast, EBU R128 (−23), Broadcast, ATSC A/85
(−24), Loud Master (−9), Level Spectrum (−14, drawn flat).

## Recipes

**Check a mix against a delivery target.** Lichen last on the master,
`render.mix`, then `fx.meters {from: "render"}`. Streaming wants about
−14 LUFS integrated and a true peak at or under −1 dBTP; broadcast −23
(EBU) or −24 (ATSC) and −1 or −2 dBTP. `overs` and `clipped` should be 0.
Fix loudness with Treeline, not by trusting the reading alone.

**Find what makes a mix muddy or harsh.** Compare `low_mid` and `high_mid`
/ `presence` with a reference's (`analyze` it, or render it through
Lichen). A `low_mid` within 3 dB of `low`, or a `presence` far above the
reference's, points to the band to cut with Yarrow.

**Check the stereo image.** `correlation` below 0, `mono_loss` above
3 LU or `low_width` above 25 % are problems: something is out of phase or
the bass is wide. Hickory's mono-below and width controls fix them.

**Write a part over existing audio.** Play or render the section and read
`key`. When `key_strength` is under 50, or two related keys are likely
(a major key and its relative minor share their notes), listen or ask the
human before committing.

## Limits

- Readings since play need a play or a render to start and stop them. In
  the DAW, `from: "live"` reads only while the engine runs; between plays
  it holds the last play's.
- The key is a guess from the pitch classes heard, not a transcription; a
  relative major and minor (C major and A minor) are the usual confusion,
  and a single note or a drum loop reads `none`.
- The integrated loudness and range are exact to a tenth of a LU (they are
  kept in histograms of that step).
- For the final check of a delivery, `analyze` the rendered file as well:
  its report adds what a meter cannot keep, such as onsets and harmonics.
