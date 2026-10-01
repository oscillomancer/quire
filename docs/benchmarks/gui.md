# GUI performance (ADR-0005 §5 gates)

**Result (2026-10-01, release build, offscreen SDL3 driver on EGL, Mesa 26.2.3
on Intel Arc B580, Core Ultra 5 225F, `powersave` governor, other workers and
builds running — load average ~16 on 10 cores):**

| Gate | Target | Measured |
|---|---|---|
| Cold start to first presented frame | < 300 ms | **101–245 ms** (empty session, 10 runs, median 161) |
| Frame time p95, 64-track session, scripted zoom/scroll | ≤ 8 ms | **0.59–0.62 ms** UI-thread CPU (max 1.24 ms, 3 × 600 frames) |
| Idle CPU (no input, no model change, 10 s) | ≈ 0 | **0.25–0.51 %** (the 100 ms service tick) |
| UI-thread tasks over 2 ms during the run | none | **0** |

Debug (`dev`) build for comparison: p95 2.8 ms (max 3.4 ms; one 3.3 ms task —
the first frame over the scripted sweep), cold start ~344 ms.

## How it is measured

```
quire --offscreen --no-socket --workdir DIR --generate 64 --script \
      --frames 600 --idle 10 --bench
quire --offscreen --no-socket --frames 1 --bench          # cold start
```

- `--generate 64` builds the session through the command bus in **one
  transaction**: 64 channel strips, each with an audio item (one of eight
  generated 30 s stereo WAVs) and an 8-bar MIDI item (4096 notes in total),
  plus a tempo change (140 BPM at bar 33), a meter change (3/4 at bar 35),
  three markers, a region, fades on every audio item, a muted item every third
  strip and a second take every third strip — so the arrange view's ruler
  lanes, item states and take badges are all exercised. Its time is included in
  the JSON's `cold_start_ms` for that run (≈ 3.0–4.4 s, dominated by writing
  the eight 30 s WAVs), so cold start is measured separately on an empty
  session.
- Counted frames start once every visible waveform pyramid is ready (the first
  run builds 8 pyramids in the background worker, ≈ 1 s; later runs load them
  from the disk cache). `--script` then changes the arrange viewport every
  frame: zoom sweeps over 8×–⅛× (5–320 px/s), horizontal pans and a full
  vertical scroll, so a frame repaints the ruler, the track control panel and
  the lanes area (waveform columns from the pyramid, item bodies, fades and
  notes as instanced GL rectangles). The playhead sits on the root's overlay
  and is not part of the counted cost.
- `frame_cpu_ms` is the UI thread's CPU time inside `Root::frame()` (layout,
  damage repaint into the cached framebuffer, GL submission, blit). It does
  not include GPU execution time; GL timer queries are a follow-up for the
  X11 path, where vsync also bounds presentation.
- Cold start: from the kernel's process start time (`/proc/self/stat`) to the
  first `present()`.

## Not covered yet

- X11 on the desktop (same code path; measure when the machine is attended).
- Meters (engine telemetry is P5.3c), automation lanes, editing drags (P5.6+).
- Playhead follow scrolling while the transport runs end to end (the overlay
  path is covered by `gui-views.tests`; a real-device run needs the JACK test
  opt-in).
