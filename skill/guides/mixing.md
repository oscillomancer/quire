# Corrective mixing

Mixing in Quire is measurement-driven: render the mix, analyze it, change one
thing, render again, `compare`. The stock EQ and dynamics plugins are in
development; until they ship, this guide covers what the engine does today —
levels, panning, muting/soloing, rendering, and measuring loudness and
masking.

## The loop

1. `session.describe` — tracks, their chains, levels and routing in one read.
2. `render.mix {analyze: "summary"}` (optionally `start_beat`/`end_beat`) writes
   the mix to a WAV and returns its analysis inline. Use
   `analyze.render {render: <render id>}` to inspect it later. Read loudness
   first (`integrated_lufs`,
   `true_peak_dbtp`), then the band levels and stereo width.
3. Run `analyze.masking {masker_track, maskee_track}` on parts that fight
   (kick and bass, bass and keys). It renders each track's post-fader output
   and reports per Bark band how much of the maskee is masked and the three
   worst bands.
4. Fix with the smallest move: level (`track.set {track, volume_db}`), pan
   (`track.set {track, pan}`), a different octave or voicing (edit the MIDI),
   or — when the EQ ships — a cut in the masked band on the less important
   part.
5. Render again and `analyze.compare {reference, test}` with the two render ids.

Group a change and its render in one `batch`: one undo step, atomic.

## Targets that are safe defaults

| goal | measure | target |
|---|---|---|
| streaming-ready level | `integrated_lufs` | −14 (±1) |
| no inter-sample clipping | `true_peak_dbtp` | ≤ −1.0 |
| mono-safe low end | `bands.low.width`, `bands.sub.width` | < 0.1 |
| no mud | `bands.low_mid.level_db` | not the loudest band unless intended |
| not harsh | `bands.high_mid.level_db`, `tilt_db_per_oct` | tilt −3..−6 for full mixes |
| mono compatibility | `stereo.mono_loss_lu` | < 1.5 LU |

These are starting points, not rules; genre and intent win. When a target
conflicts with the sound the human wants, say so and ask.

## Habits

- Change levels by small amounts (1–2 dB) and re-measure; loudness is not
  linear in perception.
- Keep the master under 0 dBTP while mixing; loudness comes at the end.
- Work on a branch (`branch.create`) so the human can A/B your mix against
  theirs and merge or discard it.
