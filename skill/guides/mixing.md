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

## Sends and buses

Use `route.add {"from":"tr_1","to":"tr_2","tap":"post_fader","volume_db":"-6 dB"}` to feed a bus track; place bus processing in that track's FX chain and use `route.list` to inspect sends. `route.set` changes a send's level, pan, tap, endpoints, or mute; `route.remove` deletes it. The destination must not feed the source, including through folder parent sends. Hardware endpoints and channel maps are not available through these commands yet.

To sidechain, route into the plugin's key: `route.add {"from":"tr_kick","fx":"fx_3","tap":"post_fader"}` (fx_3 is the compressor on the bass track; `fx.describe` shows `sidechain_channels` when a plugin has a key input, otherwise the route fails with `route.no_sidechain`). The key is delay-compensated, follows the source's mute at a post-fader tap, and keeps sounding when another track is soloed.

Use `plugin.list` to find an FX id, `plugin.scan` to refresh the catalogue, `fx.move` to reorder a chain, `fx.patch.get/set` for a semantic patch, and `fx.param.get` for one value. For arrangement edits, `item.set/split/duplicate` and `take.list/add/set/activate` change clips and takes; `marker.*` and `region.*` label the timeline, while `time.convert` maps seconds, beats, and bars.

## Exporting and comparing versions

`render.stems {mode: "isolated", tap: "post_fader"}` writes each top-level
track before master processing. Use `mode: "through_master"` to hear one stem
at a time through the master chain. `render.regions` writes named regions; a
project's `render.matrix` can select tracks or `"master"` for each region.
`render.dry-run {operation: "regions"}` lists the files and lengths first.
Patterns such as `{project}-{region_number:02}-{region|replace(" ","_")}`
name the files; names that escape the renders directory are rejected.

Use `branch.render-ab {a, b, end_beat}` to render and analyze two branch heads
without switching the active branch. For a long export, pass `job: true` to
`render.mix`, `render.stems`, or `render.regions`, then use `job.get {job}` or
`job.wait {job}` for progress and `job.cancel {job}` to stop it. WAV export
supports 16/24-bit PCM with seeded dither and 32-bit float. Set
`normalize: {mode: "lufs_i", target: -14, limit_true_peak_db: -1}` on stem or
region renders to measure the first pass and apply gain on the second.

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

## Automation

Anything that should change over time — a fade, a filter sweep, a send that
opens for a chorus — is an envelope, not a series of edits:

```
env.add {"fx": "fx_3", "param": "filter1.cutoff",
         "points": [{"bar": 9, "value": "400 Hz"},
                    {"bar": 13, "value": "4 kHz", "shape": "slow"}]}
env.add {"track": "tr_2", "param": "volume",
         "points": [{"bar": 16, "value": 0}, {"bar": 17, "value": "-60 dB"}]}
```

Mixer targets: track `volume`, `trim` (dB), `pan`, `width` (%), `mute`;
route `volume`, `pan`, `mute`. Plugin targets: the semantic keys
`fx.describe` lists (values in their unit; a cutoff sweep moves evenly in
pitch), or a CLAP id/name for other plugins. `env.points.set` replaces
points (or a beat range of them); `env.set {active: false}` bypasses an
envelope; render and `compare` to hear what it did.
