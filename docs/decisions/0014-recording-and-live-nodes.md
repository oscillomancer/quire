# ADR-0014: Recording, input monitoring and the live region

**Status:** Accepted, 2026-09-27 (recording implemented; monitoring and recovery next). Spec: `docs/specs/engine.md` §2.1, §4
(recording), `docs/specs/session-model.md` (`track.record`, routes from
`hw_input`). Builds on ADR-0012 (anticipation) and ADR-0004 (JACK inputs).

## Context

Everything renders ahead today (ADR-0012). Recording needs the device's
input captured without loss, and monitoring needs some nodes to run at the
device block size with input that exists only now — the *live region*.
Phase 4 acceptance asks for robust recording (never drop samples, disk
streaming, crash recovery) and live paths at 64 frames.

## Decision

1. **Recording first, independent of monitoring.** The audio callback copies
   each armed track's input channels (from its `record.input` route's
   `hw_input` channels) into a per-track SPSC capture ring of float frames
   sized for 10 s. The callback only copies and counts; an overflow is
   counted and reported (`record.overflow`), never silent. Capture starts at
   the render position the transport reports for that callback minus the
   device's input latency (JACK capture latency + the period), so takes land
   where they were played.
2. **A writer thread per recording session** drains the rings into WAV
   files (RF64 above 4 GiB) under `<project>/media/rec/`, rewriting the
   header every 5 s (and on stop) so a crash leaves a valid file up to the
   last rewrite. Files are named from the track and take number. On stop the
   main thread commits one version: an audio source, a take (and the item
   if new) per armed track, positioned at the capture start; punch ranges
   and loop passes (one take per pass) come with `transport.loop`.
3. **Crash recovery:** on project open, files in `media/rec/` not referenced
   by the model are offered as recovered takes (`project.recover` lists and
   imports them); the header rewrite makes them readable.
4. **The live region (monitoring).** A node is live when it monitors input
   (`record.monitor` on, or auto while armed) or is fed by a live node
   (children → folder, routes → targets, and the master). Live nodes run in
   the audio callback at the device block size, in topological order;
   everything else keeps rendering ahead. Where an anticipative node feeds a
   live one (e.g. a playback track into the master while a vocal is
   monitored), the renderer also publishes that edge's tap into an
   epoch-tagged chunk ring (as the master's today), and the callback reads
   it at the same timeline sample — anticipative audio is rendered for the
   play position, so it lines up without extra delay. The master, when live,
   is rendered in the callback from those rings plus the live nodes.
5. **Plans carry the classification.** The compiler marks nodes live or
   anticipative and lists frontier edges; a change of monitoring or arming
   is a plan swap like any other edit (live nodes switch at the swap; the
   first callback after a swap to live of a formerly anticipative node plays
   from the ring until its render position is reached, then from the live
   graph — a short crossfade avoids a click).
6. **PDC:** live paths are not delayed to align with playback (spec §1.3);
   a live node's latency is reported in `engine.graph`; recorded material is
   offset by the input latency plus the monitored chain's latency.
7. **Live multiprocessing** (engine.md §2.2) comes after: the callback
   enqueues independent live nodes to the worker pool and helps, never
   blocks; with one or two live tracks the callback alone is enough.

### Two executors over one plan (refined 2026-09-27)

- **Plans are reclaimed by generation.** The main thread publishes plans
  through an atomic pointer and keeps every published plan; a plan is freed
  once both executors (renderer and callback) have acknowledged a newer
  generation. (RtHandoff returns a plan as soon as one consumer moves on,
  which is too early with two.)
- **The renderer adopts first, the callback follows at the horizon.** The
  renderer adopts plan N at its render position R and republishes
  `(N, R)`; the callback keeps playing plan N−1 (its live nodes and N−1's
  frontier rings, which the renderer filled up to R) until its play
  position reaches R, then switches — so an edit takes effect at the same
  timeline sample on both sides, with no gap. A cycle that straddles R is
  split there.
- **Each instance has one owner at a time**, tagged on the instance when an
  executor starts processing it and cleared when it stops. An executor
  processes a stage only if it owns the instance; it starts an instance of
  its class only when the instance is not processing (the other executor
  has let go), retrying on later cycles and passing the stage through
  meanwhile. A plugin that moves between live and anticipative (arming,
  monitoring) therefore changes owner cleanly, at the cost of a pass-through
  gap of up to the render-ahead time — only at those class changes.
- **Frontier rings belong to a plan**: the renderer fills plan N's rings from
  R on; the callback reads N−1's until it switches at R.

## Consequences

- Recording works before monitoring does; monitoring adds the frontier
  rings and a second (callback) renderer over the same plan.
- The audio callback grows from "copy a ring" to "copy rings + run live
  nodes"; its RT safety is tested under rtsan with a dummy backend feeding
  synthetic input.

## Alternatives considered

Recording from the renderer thread (inputs would have to be queued to it
first — the same ring, one more hop); monitoring by running the whole graph
live while any track monitors (simple, but gives up anticipation exactly
when the machine is busiest, during a recording session).

## Implementation notes (2026-09-27)

- `host/engine/src/recording.{h,cpp}`: `FrameRing` (SPSC interleaved
  frames, 10 s), `Recorder` (capture on the audio thread; a writer thread
  drains into `audio::WavStreamWriter` — float32 WAV, header rewritten
  every 5 s and on close; a partial file is readable). `Engine::
  startRecording/stopRecording`; the callback captures only cycles that
  play timeline audio and places the first at its play position minus the
  plan's output latency and the device's output + input latency. Stopping
  waits two callbacks (none for a manual backend) before draining.
- `DummyBackend::Options::input` feeds synthetic input (tests); JACK
  connects physical capture ports and reports capture latency.
- Bus: `track.set {armed, record_input: [1-based channels], monitor}` keeps
  the hw_input route; `transport.record` records every armed track;
  `transport.stop|pause` commits one version ("record") with a media
  source, take (start offset for material before 0) and item per track, and
  returns `recorded: [{track, item, take, file, seconds, lost_frames}]`.
- Tests: contiguous capture with exact placement in both renderers; a take
  recorded over the bus plays back in a render and undoes as one step.
- Not yet: `project.recover`, punch/loop takes, RF64, monitoring (§4–5),
  and replaying a recording in a branch merge (the version has no
  invocation to replay).

