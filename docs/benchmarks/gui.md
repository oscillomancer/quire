# GUI performance (ADR-0005 §5 gates)

**Result (2026-10-01, release build, offscreen SDL3 driver on EGL, Mesa 26.2.3
on Intel Arc B580, Core Ultra 5 225F, `powersave` governor, other builds
running — load average ~7 on 10 cores):**

| Gate | Target | Measured |
|---|---|---|
| Cold start to first presented frame | < 300 ms | **79–92 ms** (empty session, 8 runs) |
| Frame time p95, 64-track session, scripted zoom/scroll | ≤ 8 ms | **0.45–0.59 ms** UI-thread CPU (max 0.72 ms) |
| Idle CPU (no input, no model change, 10 s) | ≈ 0 | **0.3 %** (the 100 ms service tick) |
| UI-thread tasks over 2 ms during the run | none | **0** |

Debug (`dev`) build for comparison: p95 2.6–2.9 ms, cold start ~400 ms.

## How it is measured

```
quire --offscreen --no-socket --workdir DIR --generate 64 --script \
      --frames 600 --idle 10 --bench
quire --offscreen --no-socket --frames 1 --bench          # cold start
```

- `--generate 64` builds the session through the command bus (64 channel
  strips, each with an audio item — eight distinct generated 30 s stereo WAVs —
  and an 8-bar MIDI item; 4096 notes in total). Its time is included in the
  JSON's `cold_start_ms` for that run (≈ 2.1 s), so cold start is measured
  separately on an empty session.
- Counted frames start once every visible waveform pyramid is ready (the
  first run builds 8 pyramids in the background worker, ≈ 1 s total; later
  runs load them from the disk cache). `--script` then changes the lanes
  viewport every frame: zoom sweeps over 8×–⅛× (5–320 px/s), horizontal pans
  and a full vertical scroll, so every frame repaints the whole lanes area
  (waveform columns from the pyramid, item bodies and notes as instanced GL
  rectangles).
- `frame_cpu_ms` is the UI thread's CPU time inside `Root::frame()` (layout,
  damage repaint into the cached framebuffer, GL submission, blit). It does
  not include GPU execution time; GL timer queries are a follow-up for the
  X11 path, where vsync also bounds presentation.
- Cold start: from the kernel's process start time (`/proc/self/stat`) to the
  first `present()`.

## Not covered yet

- X11 on the desktop (same code path; measure when the machine is attended).
- Meters (engine telemetry is P5.3c), automation lanes, drags (P5.6+).
