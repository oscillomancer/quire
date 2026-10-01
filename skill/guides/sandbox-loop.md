# The sandbox loop: program a plugin by measuring it

The sandbox is a headless host: load a plugin, set it in real units, play a
phrase through it offline, and read an analysis report. Renders are
deterministic — the same patch and phrase always produce the same audio, so
differences you measure are differences you made.

## The loop

1. **Load and read.** `plugin_list` → `plugin_load {plugin}` →
   `plugin_describe {instance, detail: "summary"}`. The description lists
   parameter keys, units and ranges, and the patch's current prose summary. With
   `detail: "full"` a parameter whose module is not in the patch carries
   `"active": false` — add the module with `patch_merge` before using it.
2. **Decide the structure first** (which modules, which routes), then set a
   first guess in one `patch_merge`. For Orrery read `guides/sound-design.md`.
3. **Render a phrase that exercises the sound** — a held note for pads and
   bells, repeated short notes for plucks and basses, the real riff if you
   have one:
   ```json
   {"notation": "notation 1\nvoice v ch 1\n1: C2:e r:e C2:e Eb2:e r:e G1:e Bb1:q", "analyze": "summary"}
   ```
4. **Read the report** and compare it with what the sound should be (see
   `guides/analysis.md` for what each number means): brightness is
   `spectral.centroid_hz` and the band levels; snap is `envelope.attack_ms`
   and `decay20_ms`; weight is the `sub`/`low` bands; width is
   `stereo.width`; harmonic character is `harmonic.harmonics_db`.
5. **Change one idea at a time** with `patch_merge`, render again, and
   `compare {reference: <previous render>, test: <new render>}` to see
   exactly what moved.
6. **When the structure is right but the numbers aren't**, stop nudging and
   run `optimize_start` with the few parameters that matter and a target
   (a reference render/file, or a profile of descriptor ranges). Poll with
   `job_wait`. Apply the result with `patch_merge` (or `"apply": true`).
7. **Save** with `preset_save {name, tags, notes}` and tell the human what
   you aimed for, what the numbers show, and which render to listen to.

## Habits

- Read `patch_get {detail: "changed"}` before editing; it is short.
- Batch a merge and its render: one `batch` call, atomic, fewer round trips.
- Keep render names meaningful (`"name": "pluck-v3-shorter-decay"`) — the
  human will listen to them.
- If an error comes back, its `hint` is the fix. Apply it; don't guess.
- Loudness is not quality. Compare sounds at matched loudness (`compare`
  already level-matches timbre terms) and never "improve" a sound by making
  it louder.
