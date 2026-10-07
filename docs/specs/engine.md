# Spec: the headless engine (Phase 4)

**Status:** Draft for review (2026-09-26). Frozen before P4 implementation.
**Decision records:** ADR-0004 (backends), ADR-0008 (model/plan
separation), ARCHITECTURE §3 (threads, channels, RT rules, main-thread
budget), §6 (bridging, latency changes).
**Code:** `host/engine` (`supple::engine`), `host/isolation` (bridge
process), reusing `host/clap-host` from Phase 2 unchanged.

The engine turns immutable session versions into sound: in real time through
an audio backend, or offline as fast as possible. Its non-negotiables are
real-time safety, determinism, and REAPER-class efficiency (anticipative
processing, flexible isolation).

---

## 1. Compilation: Session version → RenderPlan

The model thread (or a worker it delegates to) compiles a version into an
immutable `RenderPlan`. Plans are never edited; a new version means a new
plan (built incrementally from the previous one where possible, §1.6).

### 1.1 Nodes

- **StripNode** per track *(amended 2026-09-30, ADR-0019)*, stages by strip type:
  channel `inputs → [tap: pre_fx] → items/takes → inserts → mono/polarity →
  [tap: pre_fader] → C9 Channel (fader, pan, mute) → [tap: post_fader]`; buss
  `input sum → C9 Buss → [pre_fx] → inserts → mono/polarity → [pre_fader] →
  C9 Channel → [post_fader]`; master `input sum → C9 Buss → [pre_fx] →
  inserts → mono/polarity → [pre_fader] → plain gain/pan → [post_fader]` →
  outputs/renders/monitor chain. Mono and polarity (ADR-0022) act on the
  main pair, ramped like mute, never between an encode and a decode. With
  `project.console9` off the C9 stages run linear (same gains and pan law,
  no curves, noise or dither; ADR-0019 §6). Items play *before* the
  chain (as in REAPER); input monitoring and record inputs enter channels
  at `inputs`.
- **SendEncoder** per route into a buss/master that cannot share the
  source's `post_fader` tap: the route's polarity/mono, then its own C9
  Channel (fader = send level, plus the strip's fader for post-fader
  sends; pan = send pan or the strip's), then the edge. Edges into a buss
  or master only sum, channel-map and delay (ADR-0019 §3); a plan-level
  checker asserts it in debug builds and in the strip fuzz test.
- **ItemPlayer** (inside a TrackNode): per active item on the track's lanes,
  per take: source reader (audio: disk-streamed decoded+resampled blocks
  from the media cache; MIDI: event generator from the source with playrate
  and loop), take FX chain, take envelopes, item gain/fades/crossfades.
- **FxInstance** wrappers around `host::Instance` (in-process) or a bridge
  proxy (§5), with wet/dry, bypass, pins, oversampling, parallel groups and
  containers compiled into a small per-chain program.
- **RouteEdge**: copy from a source tap with gain/pan/channel map (and MIDI
  filter), plus its PDC delay line.
- **HwInput / HwOutput** nodes bound to backend ports.

Looping MIDI takes wrap `start_offset` modulo the source's beat length, as audio
loops do. Compilation covers the initial partial cycle, every full repetition,
and the final partial cycle; notes crossing the item's start are retriggered
there and their note-offs remain correctly placed. SMF export is limited to
16 MiB of encoded bytes, including long-delta padding; larger output returns
`midi.smf_too_large` before padding is emitted.

### 1.2 Buffers

All buffers are allocated at compile time: per node, `channels × maxBlock`
float per tap in use, one input-sum buffer, per-edge delay lines sized for
the edge's compensation, per-plugin scratch for pins/oversampling. MIDI/event
buffers are fixed-capacity arenas (overflow drops and counts).
Summation of a node's inputs happens **in a fixed order** (edges sorted by
source node order, then edge ID) after all inputs for the block exist —
never by concurrent accumulation — so results are thread-count independent.

### 1.3 Plugin delay compensation

- Each FX instance reports latency (CLAP `latency`); bypassed instances keep
  their latency (compensated bypass); offline instances report 0.
- A chain's latency: sum over serial stages, max over a parallel group
  (shorter branches delayed to match inside the chain program).
- For every node, `arrival(e) = latency(source up to tap(e)) + edge delay`
  must be equal for all inputs; the compiler solves this by giving each edge
  `delay(e) = max_in − latency_at_tap(e)`, recursively from sources to the
  master, then shifts the transport so the master output is aligned (the
  plan's `output_latency`). Items on a track are read `latency` samples
  early so they line up at the output. Pre-FX taps from a track with
  latency get delayed like any other.
- `fx.pdc = ignore` excludes an instance from compensation (for low-latency
  monitoring chains).
- Input monitoring paths are not delayed to align with playback (they are
  live); recorded material is offset by the backend's reported input latency
  plus the monitored chain's latency so it lands where it was played.
- Runtime latency changes follow ARCHITECTURE §6: `request_restart` → new
  plan built in the background → swap at a block boundary with a 10 ms
  crossfade on affected paths. As built (2026-10-06): a restart the plug-in
  asks for while processing (CLAP `request_restart`, VST3 `kLatencyChanged`,
  e.g. a "Low Latency" mode) is `Instance::restartPending()`; the pool marks it
  `restarting`, the engine reloads, and that plan leaves the instance off both
  executors' instance lists (its stage passes through, compensated). Once the
  executors have adopted it the instance is no longer processing, the next
  `InstancePool::service()` restarts the same instance (its state kept), and a
  latency that moved since the last service (`latencyChanged`) reloads the plan
  with the new compensation.

### 1.4 Automation

Envelope points are pre-converted to plan time (samples at the engine rate,
per the time map; beat-timebase points are converted through the tempo map
at compile time). Per block, each automated target evaluates its segments:
- mixer targets (volume, pan, mute, send level/pan) interpolate per sample
  (gain ramps) and drive the C9 Channel (or the plain stage) — cheap and
  exact; Console 9 stages restart their seeded dither at every locate;
- plugin parameters receive CLAP param-value events at segment points and
  every `automation_resolution` samples inside curved/linear segments
  (default 32; always at exact point positions), in the parameter's CLAP
  domain (normalized for stock plugins, ADR-0010);
- write/touch/latch modes record from GUI/control input into the model on
  the main thread (reliable coalescing channel, ARCHITECTURE §3), creating
  one version per gesture.

### 1.5 Transport and time

The plan holds a sample-domain time map (tempo segments → samples) so the RT
thread computes CLAP transport (beats, bars, tempo, time signature, loop,
playing/recording flags) per block without floating-point drift (positions
are integer samples; beat positions derived per block from the segment
table). Looping wraps at exact samples; a block that crosses the loop point is
split.

Time conversions clamp at their representable limits: `samplesAt` saturates
rounded products to signed 64-bit samples (NaN maps to zero); `barBeat` clamps
to signed-int bars including the last fraction of the final bar (NaN selects
beat zero). Callers must still validate finite positions and leave room for
block and latency arithmetic. JACK validates requested rates in 1..768000 Hz
before constructing its latency request.

### 1.6 Incremental rebuild and plan swap

The compiler diffs the new version against the one the current plan was built
from (session-model §4). Unchanged nodes keep their plugin instances and
state (their DSP objects are moved into the new plan). Held timeline notes
carry only when the receiving instrument instance is retained. Route and parent
edge delay histories carry when source, destination, tap, kind and delay length
match; changed lengths start empty. New plugins are
instantiated and activated on the main thread before the swap; parameter
changes on existing instances become param events rather than rebuilds.
The swap: `std::atomic<const RenderPlan*>` exchanged by the model thread;
the RT thread picks up the new pointer at the next cycle start and
acknowledges via an epoch counter; the model thread destroys the old plan
(and deactivates removed plugins) only after the acknowledgement. Nothing on
the RT thread frees memory.

## 2. Real-time execution

### 2.1 Live and anticipative regions

A node is **live** when its output this cycle depends on something only
known now: hardware inputs (armed or input-monitored tracks), the MIDI input
for a monitored instrument, or a live-flagged track (plugin GUI open —
configurable, default on; open MIDI editor with preview — configurable). Live
propagates downstream: any node fed by a live node is live (the master is
live whenever anything is live). All other nodes are **anticipative**.

- **Anticipative nodes** render ahead of the playhead on worker threads into
  per-node SPSC ring buffers, `render_ahead` long (default **200 ms**,
  REAPER's default), in larger blocks (default 1024 samples). The RT thread
  consumes their output at device block size. Edits that reach an
  anticipative node (parameter changes from the UI) take effect at the
  render-ahead horizon — like REAPER — unless the node is live-flagged.
  Seeking or a structural edit flushes the rings and restarts render-ahead
  from the new position (brief pre-roll; the RT thread outputs silence for
  a node whose ring is not yet filled, counted as an anticipation miss).
- **Live nodes** run inside the audio callback at device block size, in
  topological order, parallelized over the worker pool when
  `live_multiprocessing` is enabled (default: on when the live region has
  ≥ 2 independent nodes of meaningful cost).

Eligibility knobs mirror REAPER's (research doc, Addendum 2): anticipate
tracks without FX (default off: nothing to gain), tracks with open MIDI
editors (default off), tracks in touch/latch/write automation (default on).
The current classification of every node is queryable
(`engine.graph {detail}`) and emitted in engine stats.

### 2.2 Worker pool

`N = max(1, cores − 2)` workers by default (10 cores here → 8), each with a
lock-free work-stealing deque; tasks are `(node, block range)` with atomic
dependency counters. Workers sleep on a futex when idle; wakes use
`rt::wake()` (the one sanctioned RT syscall). Anticipative work runs at
normal-but-elevated priority; live work at RT priority (SCHED_FIFO via rtkit
if available, reported otherwise — never by changing system config).

The RT thread never blocks on a worker: for live parallelism it enqueues
tasks, then **helps** (pops and runs tasks itself); it spins only on tasks
already in progress elsewhere, up to a deadline (default 50 % of the block
period); if a live task misses the deadline the RT thread finishes that
cycle without it (the node outputs silence for that block, counted as a
live-scheduling miss — this should never happen in normal operation and is
a benchmark failure).

### 2.3 The RT cycle

1. Pick up a new plan if published (acknowledge epoch).
2. Drain the model→RT command queue (transport, param events, meters on/off).
3. Read hardware inputs into HwInput buffers; feed record rings.
4. Run live nodes (with worker help); pull anticipative outputs from rings.
5. Sum into the master, run the master chain (live if anything is live, else
   the master is anticipative too and the RT thread only copies its ring
   output).
6. Write hardware outputs; publish meters (lossy rings) and state deltas
   (coalescing slots).

**Meters** (P5.3c). Per strip, the peak and the mean square of the strip's signal
**after the fader and after the pan law, before the Console 9 encode**
(`sum × faderGain × ` the stage's own `panGains` — what a user expects at
unity: a −6 dBFS item at 0 dB reads its own peak times the Channel pan law)
since the reader's last call; a master has no encode and measures its output,
the signal the device is given. Two SPSC rings of `MeterEntry` carry the
numbers, one per producer: the renderer thread publishes what it renders ahead
(after the block, collected from its nodes, never from worker tasks), the audio
callback publishes what it plays — the live nodes, or the whole graph when
anticipation is off. An entry is **heard** once the callback's play position in
that epoch has passed the end of the block it covers, minus the device's
reported output latency; entries of an older epoch (a seek or a pause bumped
it) are dropped, and while the transport is stopped everything already
processed counts as heard (monitored input). `EngineApp::meters()` drains the
rings and returns one reading per strip of the newest plan — zeros for a strip
with no heard audio; decay and peak hold belong to the GUI. Each entry carries
its slot generation from the publishing plan; entries from a retired allocation
cannot contaminate a newer track even when loads outrun plan adoption.
A full ring drops
the entry and counts it (`engine.status.meter_drops`, per producer). Offline
renders do not meter at all (no bank: zero cost), and the publishers never
touch the signal, so renders stay bit-identical.

Everything in this cycle is `SUPPLE_RT` and runs under rtsan in the `rtsan`
preset's engine tests (with the dummy backend).

## 3. Offline rendering

- Runs a **separate plan instance** with its **own plugin instances**
  (created from the version's state/patches), so rendering never disturbs
  playback and A/B renders of two branches can run side by side.
- Fixed block size (default 512; setting), no anticipative split, all nodes
  in topological order, optionally parallel across independent nodes —
  results must be **bit-identical for any worker count** (fixed summation
  order, §1.2) and across repeated runs (acceptance test).
- Real-time/offline match: for sessions of stock plugins (block-size
  invariant by their own acceptance tests), a real-time capture through the
  dummy backend at any device block size nulls against the offline render to
  −120 dBFS (acceptance test). Third-party plugins that are not block-size
  invariant are documented exceptions.
- Render jobs (`render.*`): mix, stems (per track post-fader through the
  master-less path, or through master with others muted — option), regions,
  the render matrix, selected items; tail handling (`tail_ms` or until
  silence); normalization (loudness via core-analysis, then gain, then
  optional true-peak limiter — deterministic, two-pass); dither (seeded
  TPDF); formats WAV (PCM/float, RF64 beyond 4 GiB) and FLAC (libFLAC,
  BSD-3, vendored when needed — not yet: `render.*` writes WAV only); file
  naming per session-model §7.3; a job
  queue; results reported as files + analysis summaries.

## 4. Media, recording, device handling

- **Media cache**: decoded and resampled audio (Signalsmith-quality
  resampler or the audio-file polyphase resampler), keyed by `(content hash,
  engine rate)`, disk-backed under `$XDG_CACHE_HOME/<slug>/media/`. Disk
  streaming readers prefetch on I/O threads into per-item rings (default
  1200 ms like REAPER's media buffer).
- **Peaks**: min/max pyramids built on I/O threads, cached by content hash,
  never in project diffs.
- **Content hash**: `MediaSource.hash` is `"sha256:<hex>"` of a file's bytes —
  read with the file when it is imported (`item.add`, media import). Recording
  stop drains capture and closes the WAV (final header sizes only); it commits
  takes immediately as one "record" undo step with empty hashes. An existing
  job worker rereads the closed file, then a worker finalizes its sidecar at
  the current location (including a first save moving scratch media). The
  model-thread completion fills matching media in every stored version without
  changing version IDs, object revisions or undo/redo history; already leased
  snapshots stay immutable. Transactions opened before completion retain the
  hash on commit. If the originating branch has undone/deleted the take, or the
  project was replaced, the update is dropped. A save while the job is pending
  writes a valid empty hash. Hash/read/sidecar failures leave it empty and emit
  `bus.post_failed` with a structured error. Adding the same bytes again reuses
  a media source whose hash is already known. Metadata for an import comes from the container's header
  (`audio::inspect`), never a decode: adding one file to many items costs one
  header read at a time. Projects saved before the hash existed keep their empty
  hashes (the peak cache falls back to path + size + mtime); adding one of their
  files again reuses its source — the path and the header it was added from
  decide — and fills the hash with the bytes now on disk, so such a project
  converges instead of growing a second source for one file. A file is hashed
  the same way when it is recorded.
- **Recording**: RT thread writes input frames into large preallocated SPSC
  rings (≥ 10 s); an I/O thread writes them to WAV/RF64 files incrementally,
  rewriting the header every 5 s so a crash leaves a valid file up to the last
  flush; a sidecar per file records its track and start, and
  `project.recover` repairs the header to cover everything on disk and places
  takes a crash left out (ADR-0014); punch in/out and loop recording (one take per pass); MIDI recording
  into MIDI takes. Audio ring overflow becomes equal-length silence at its file
  position and is still reported; loop run positions remain in file-time units.
  Disk and periodic header errors stop only the failing target and retain that
  target's first error. The loop run table holds the initial run plus 4096 wraps;
  when full, capture stops adding input and returns retained passes with a
  structured `recording.loop_runs_full` warning for the commit. Sidecars require
  `supple.recording/1.0`, a track id, a positive finite sample rate, and complete,
  ordered loop runs beginning at file frame zero; invalid sidecars return a
  structured error and must be skipped during recovery. Placement sidecars
  survive pending hash jobs (including finalized loop runs at stop); the
  background finalization adds an optional `hash` without changing their
  recovery contract.
- **Backends** (ADR-0004): JACK (dlopen), dummy, offline in P4; buffer-size,
  sample-rate and device changes stop the engine cleanly, rebuild the plan,
  and restart; xruns are counted (backend callbacks + our own deadline
  misses) and emitted as `engine.xrun` events with context (block size,
  load, which node was slowest).

## 5. Plugin isolation (`host/isolation`)

Modes per instance (session-model §1.7): `in_process`, `shared_bridge` (one
bridge process for all instances in that mode), `dedicated` (one process per
instance).

- The bridge executable (`<slug>-bridge`) hosts plugins with the same
  `clap-host` library. Control plane: a Unix socket speaking a small binary
  protocol for main-thread calls (activate, params, state, semantic
  extension, GUI create/set_parent with the host's X11 XID, timers).
  Data plane: a `memfd` shared-memory region per instance with input/output
  audio buffers, event arenas and a cycle header; wake via eventfd/futex
  through `rt::wake()`.
- The current bridge is synchronous and runs only offline or on rendered-ahead
  nodes. A stage on a device-callback node (including a take FX stage) passes
  through with its compensated delay; the callback calls neither processing nor
  lifecycle methods of the bridge. The plan warning names the FX and asks for
  `isolation: in_process` to monitor through it. Pipelined live bridging is future
  work (ADR-0013); synchronous monitoring is not supported.
- Crash handling: a control thread watches the process (pidfd); on crash the
  instance becomes a **failed node** (silence; `fx.failed` event with the
  signal and last known state kept in the model), the rest of the session
  keeps playing, and `fx.restart` re-creates it from the last known state.
- Acceptance: a deliberately crashing test plugin (a CLAP that segfaults on
  a given parameter value) in `dedicated` mode does not stop the engine; a
  real-time capture around the crash shows only that node going silent.

## 6. Control surface

The engine registers the session-model §7 command catalogue on the command
bus and exposes it through the control protocol (Unix socket), Lua (Phase
4/5 bindings) and MCP (proxy or in-process). Engine-specific commands:
`engine.start {backend, device, rate, block}`, `engine.stop`,
`engine.status` (q: backend, rate, block, load, xruns, anticipation
misses, meter drops), `engine.graph` (q: nodes, live/anticipative classification,
latencies, CPU per node), `engine.settings.set {render_ahead_ms,
worker_threads, live_multiprocessing, automation_resolution, …}`.
Events: `transport.changed`, `engine.xrun`, `engine.stats` (1 Hz),
`meters` (rate-limited, subscribe per track), `fx.failed`,
`record.progress`.

The servers drive a **service tick** on the model thread every 100 ms
(`EngineApp::tick()`; `ModelLoop::every` on the socket server, a poll
deadline in the stdio MCP loop), so the app's periodic work happens with no
client asking: retired plans are freed and `fx.failed` is emitted (that is
why subscribers see failures on their own), the project is autosaved when
its head changed (session-model §5.3), and a device change restarts the
engine — `engine.restarting {reason}` when the running backend reports
`restart_needed`, then `engine.stop`/`engine.start` with the same backend
kind, block and options, retried with backoff for up to 30 s and finally
reported as `engine.error {reason, error}`. The transport position and the
loop are restored; playback stays stopped.

## 7. Benchmarks (acceptance, `docs/benchmarks/`)

- **B1 capacity:** generated session: N tracks, each an Orrery instance
  (standard patch from `docs/specs/synth.md` §9) + stock EQ + stock
  compressor playing a 4-bar notation loop, one armed audio track with the
  same chain monitoring input; at 48 kHz / 64 frames on this machine: find
  the largest N with zero xruns over 5 minutes, with and without
  anticipative processing. Target: anticipative on ≥ 3× the tracks of
  anticipative off.
- **B2 REAPER comparison:** the same structure in REAPER 7 (the stock synth
  loaded as CLAP in REAPER, ReaEQ/ReaComp substituted where needed and
  noted), same device settings, anticipative FX at its default; report N for
  both.
- **B3 latency/PDC:** impulse through chains with known latencies across a
  folder/send topology; outputs must align to the sample.
- **B4 determinism:** repeated offline renders (1, 2, 8 worker threads)
  byte-identical; real-time capture vs offline null.
- **B5 edit responsiveness:** 1000 random edits via the bus while playing:
  main-thread time per edit (p95 < 2 ms), no xruns, plan swap latency.
- **B6 isolation:** crash test (§5) + CPU overhead of pipelined bridge vs
  in-process for 16 instances.

Results are recorded with kernel, PipeWire version, quantum actually
granted, and CPU governor (read-only facts).

B1/B2 tooling: [tools/bench/README.md](../../tools/bench/README.md).
The command-built shared session, paced search and REAPER offline generator
are dry-verifiable. Real measurements require `SUPPLE_AUDIO_DEVICE_TEST=1`
exactly; no real-device acceptance numbers have been recorded yet. The
search retains every trial, independently confirms its candidate, and labels
a passing search cap as a lower bound. REAPER counts observed audio/media
underrun timestamp changes (multiple events between polls can be undercounted).

## Implementation notes (2026-09-26, orchestrator)

- **Code layout:** `host/engine/src/plan.{h,cpp}` is the shared compiled plan
  (compile on the main thread, prepare = activation/latency/buffers/events,
  `processBlock` and `locate` on the audio thread); `offline.cpp` drives it
  for `renderOffline`, `realtime.cpp` for the `Engine`. Real-time output is
  bit-identical to the offline render for block-size-invariant plugins
  (tested at 128-frame device blocks against 512-frame offline blocks, with
  and without plugin latency).
- **Plan hand-off:** `RtHandoff<Plan>`. `acquire()` returns the previous plan
  to the main thread immediately, so the audio thread keeps its own
  preallocated list of the instances it is processing; on adoption it stops
  instances the new plan dropped and starts new ones, then stores the plan's
  generation; the main thread prunes the instance pool only once that
  generation reaches the newest published one (`service()`), so no instance
  is destroyed before the audio thread has stopped it. A plugin whose
  `start_processing()` fails is left out of processing (its stage passes
  audio through, delay-compensated, and `processErrors` counts it); the next
  plan retries it. Per-stage event storage is reserved for the most events
  any block-long window holds, so a seek off the block grid never allocates.
  Plans share ownership of the session's TimeMap (a session version may be
  freed while its plan plays).
- **Transport:** play/pause/seek/shutdown are coalescing atomics (the latest
  request wins, none is dropped however many arrive between callbacks).
  Shutdown: `Backend::stop()` promises one final zero-frame callback on the
  audio thread (a manual driver makes it on its caller, which is its audio
  thread), and the engine stops every plugin there — no timeout, no stop from
  the main thread however long the device's period. Pause and
  seek `reset()` the plugins (no hanging notes); seek re-locates event and
  automation cursors and clears delay lines. The live output lags the
  render position by the plan's output latency.
- **Mixing decisions:** instruments add their output to the track's input;
  effects mix wet/dry against the input delayed by their latency; bypass is
  compensated (a delay of the stage's latency); the pan law applies to tracks,
  the master pans as a balance; mute silences the post-fader tap only; a
  solo-silenced track is still processed. *(Superseded by ADR-0019 for
  strips: Console 9 stages, mute as a fader ramp before the encode.)*
- **JACK backend** (`jack.h`, `jack_backend.cpp`): libjack is dlopen'ed
  (default `libjack.so.0`) through our own declarations of the API subset we
  use, loaded once per path and never unloaded. The engine block is the
  largest callback: a device period larger than the block is split into
  block-sized calls (an odd period leaves a shorter last call), so period
  changes need no restart. The block is requested from PipeWire as the
  client's latency (`PIPEWIRE_LATENCY=<block>/<rate>`, set only for the
  `jack_client_open` call and only when the user has not set it) — a request
  the graph honours unless another client needs a longer period;
  `PIPEWIRE_QUANTUM` is not used because pipewire-jack maps it to
  `node.force-quantum` + `node.force-rate` (the whole graph).
  `forceBufferSize` (`jack_set_buffer_size`) is the opt-in for benchmarks.
  The granted rate is the engine's rate; a project at another rate plays at
  the device's (positions are in seconds, media is resampled to the plan's
  rate, plugins are activated at it; renders stay at the project's rate). Stop:
  the next process cycle makes the final zero-frame callback; if the server
  is gone or no cycle comes within 2 s, the client is deactivated and closed
  and the engine stops its plugins on the main thread. Xruns (server
  callback), rate changes and server shutdown are atomics read by
  `engine.status` (`restart_needed`); the model thread's service tick (§6)
  restarts the engine after a device change. Tests use a stand-in libjack
  (`tests/fake_jack.cpp`) that drives the
  process callback from its own thread and injects period/rate changes,
  xruns, stalls and shutdown; the real server is used only when
  `SUPPLE_AUDIO_DEVICE_TEST` equals exactly `1` (unset, empty, `0`, and
  all other values skip it):
  `SUPPLE_AUDIO_DEVICE_TEST=1 engine.tests -tc='real JACK*'` (2 s of silence
  at a 64-frame block). First run on this machine: 48 kHz, period 128 for a
  64-frame request, 0 xruns, final callback on the JACK thread.
- **Control:** `engine.start {backend: jack|dummy, block}`, `engine.stop`,
  `engine.status`, `transport.play|pause|stop|seek|get`. Transport actions
  wait (≤ 1 s) for two audio cycles so their answer is the new state;
  `transport.stop` returns to where play started. `<slug>-engine play FILE`
  plays a project through the device from the CLI.
- **Anticipative processing** (ADR-0012, `EngineOptions`, default on): a
  renderer thread owns the plan, the plugin lifecycle and the render
  position, renders the whole graph (nothing is live yet) in 1024-frame
  blocks into an SPSC ring of epoch-tagged chunks kept 200 ms ahead, and the
  callback only copies chunk frames. Seek and pause bump the epoch on the
  main thread; the callback drops older chunks at its next cycle (pause is
  silent at once, a seek never plays stale audio); the renderer relocates —
  after a pause to the position the callback reports once it has seen the
  new epoch. New plans are adopted at the render position (edits are heard
  after the audio already rendered, no gap). The renderer paces itself by
  the frames rendered ahead of the played position and naps otherwise; the
  callback makes no system call. Misses (no chunk while playing, after the
  pre-roll) are counted (`engine.status.anticipation_misses`). `stop()`
  joins the renderer first; it stops every plugin as it ends. With
  `anticipative = false` the old in-callback renderer runs (the reference,
  and the base for live nodes).
- **Worker pool** (`workers.{h,cpp}`): helper threads (every core but three,
  plus the submitting thread) run one render block as a DAG over the plan's
  nodes: per-node atomic counts of unfinished sources, a bounded Vyukov MPMC
  ready queue, the submitter helps; idle helpers park on an atomic wait.
  `processNode` is the unit (a node sums its inputs in the fixed order and
  processes its own edges, items and chain), so results are bit-identical
  for any thread count — the offline renderer uses the pool too (B4 test:
  1, 2, 8 threads byte-identical) and defaults to all cores.
- **Plugin isolation** (ADR-0013): `dedicated` fx run in `<slug>-bridge`
  processes (control socketpair + memfd data plane + futex, synchronous with
  a 2 s deadline (lifecycle commands 7 s)); a crash or hang fails only that instance (silent node,
  `fx.failed`, `engine.status.failed_fx`); `fx.restart` re-creates it from
  the model. Acceptance test in `engine.tests`.
- **Plugin-parameter automation:** an envelope on `fx_* <param>` resolves the
  parameter from the plugin's semantic schema (`params::fromDescription`
  rebuilds its ParamSpec, so the host converts exactly as the plugin does)
  or, for any plugin, by CLAP id or name (plain CLAP values). Semantic points
  are physical and interpolated in the normalized (curve) domain (ADR-0010).
  The stage gets param events at absolute timeline sample positions that are
  multiples of 32 in the lane's latency-shifted coordinates, and at each exact
  point (in time order; unchanged values are not resent). A block-start value
  is sent only to initialize a lane after prepare, locate, seek or loop wrap,
  merged into its sorted event list by an allocation-free insertion sort,
  within capacity reserved at prepare. Lanes shift by the latency ahead of
  the stage; seeks reset them and resend. Real-time output equals the
  offline render. Commands: `env.add`, `env.points.set`, `env.set`,
  `env.remove`, `env.list` (values in the parameter's unit; text like
  "800 Hz" for plugins). Write/touch/latch recording is not implemented.
- **Recording and input monitoring** (ADR-0014): see the ADR's
  implementation notes (recording; the live region with two executors;
  crash recovery).
- **Loop and punch** (ADR-0015): see the ADR's implementation notes.
- **MIDI input** (ADR-0016): see the ADR's implementation notes.
- **Smooth swaps:** a plan swap carries each strip's fader ramps and Console 9 state
  and each insert's applied wet level into the new plan
  (`carryMix`, by track and by instance, per executor); a changed volume,
  pan, mute, solo, FX wet or bypass then ramps over 10 ms instead of
  stepping (a bypassed plugin keeps running until faded out). Automated
  values follow their envelope; mute/solo retain a separate 10 ms ramp before
  encoding. A Console 9 mode change crossfades each implicit stage over 10 ms,
  keeping user inserts on their single processing path. Dither states persist
  through edits and restart on transport locates; source-owned send encoders
  run before compensation delays and frontier rings. Channel/buss post-fader
  stems get a neutral decoder. Structural changes (a plugin inserted or
  removed, a route added) still switch at the swap.
- **Media resampling:** `MediaCache::get(path, rate)` decodes and resamples
  once per (file, rate) with the audio-file polyphase resampler (Kaiser
  windowed sinc; deterministic, so offline ≡ real time); plans ask for their
  own rate. The real-time engine compiles at the device's rate, so a project
  at another rate plays (previously refused). The cache is in memory and the
  resampling is synchronous at plan compile — the disk-backed, content-hashed
  cache and background decode above are still to come.
- **Take playrate and pitch:** a take's playback variant is built once per
  (file, plan rate, variant) in the same cache: varispeed resamples to
  `rate / speed` (`pitch_mode: "resample"` or `preserve_pitch: false`), the
  default `"signalsmith"` mode stretches with Signalsmith Stretch (MIT,
  seeded, so deterministic) and transposes by `pitch`. The stretch runs at
  plan compile on the main thread (~0.3 s per source second in a debug
  build); it moves to the background decode with the disk cache.
  `stretch_markers` are not applied yet (the compiler warns).
- **Not yet:** live multiprocessing,
  one shared process for `shared_bridge`,
  MIDI output ports, automation recording
  (write/touch/latch),
  crossfades for structural changes (inserting/removing plugins), B1/B2
  real-device acceptance measurements (tooling: tools/bench/README.md;
  historical Orrery-only measurements: docs/benchmarks/b1-preliminary.md).

### Recording diagnostics (2026-10-04)

A committed recording may carry a `warnings` array with structured
`code`, `message`, `path`, `hint`, `track` and `file` fields.
`recording.loop_runs_full` is forwarded to `transport.stop` / pause callers
and the `record.warning` event while retained passes still commit.
`project.recoverable` and `project.recover` expose skipped invalid-sidecar
diagnostics in the same optional `warnings` shape (`record.sidecar`). Recovery
skips corrupt selections and continues with valid recordings; an unknown path
still fails, and a directory with no recordings or diagnostics reports
`recover.nothing`.

### Plug-in notifications and output events (review round 3)

The shared `model::SourceBeatMapping` defines MIDI take offset and playrate
conversion for playback and thumbnails. Looped offsets are cyclic modulo the
source length; playback includes the preceding cycle's crossing notes, chases
notes held at item start, and clips releases to the item end. Drawing retains
its bounded visible-pass enumeration; that drawing limit does not cap playback.

`InstancePool::service(fx)` services owned instances on main and drains
`restartError`. A refused reactivation is retained in `failedErrors()` as
`{fx, Error{code,message,path,hint}}`, also appearing in the existing string
`failed()` API for compatibility. Its stage passes through with the last active
latency; preparation does not retry it. `fx.restart` replaces it and clears the
failure. The app still needs to publish the structured error in `fx.failed`
and engine status (its current consumer publishes only the reason string).

Local CLAP `request_process` is an atomic wake request. The audio owner consumes
it at the next processed block. With `auto_bypass_on_silence`, a stage may idle
only after `CLAP_PROCESS_SLEEP`, with exactly silent main/key inputs and no
input events or pending flush. Notes, automation, nonzero input, queued main
parameter changes and `request_process` resume it. A stopped transport keeps
running every plugin without advancing its position (ADR-0014): the callback's
plan each cycle, the renderer's share at the device's pace (a request runs at
once); play re-locates the non-live nodes and resets their plugins.
A plain plugin's state reaches the model when it calls `clap.state
mark_dirty` (a coalesced "Plug-in state" step per plugin) and, for every
running plugin, before each save: a sampler's loaded files are saved even
when it never says so (LSP's multi-samplers do not). An inactive
instance is activated on main using its last activation configuration; a never
activated instance is configured by plan preparation. Explicit bypass is kept; queued note/parameter events are still processed with
compensated dry output so note releases are not lost while bypassed.
Bridged wake and clear forwarding need the control-protocol extension below.

Rescan requests rebuild an immutable plan from the current parameter list and
schema; envelopes are resolved again, including their value-domain conversion.
Missing/read-only parameters stop receiving events and produce a warning naming
the FX and parameter. Clear `ALL`/`AUTOMATIONS` suppresses only the matching
parameter's automation until a subsequent rescan or explicit engine load. Clear
`MODULATIONS` has no automation effect (the engine has no modulation lanes).
Envelopes remain in the model. A rescan after a clear in the same service batch
re-enables it; a clear after a rescan remains effective. Changes are heard at
normal plan-adoption boundaries, including the anticipative edit horizon.

`Engine::takePluginOutputs()` is main-thread only. Each record contains FX id,
plan generation, transport epoch, timeline sample, stream sample, the entire
`host::OutputEvent`, and idle/anticipative markers. Timeline coordinates subtract
the stage's input PDC; idle flush records use the service-time play position.
Parameter values and gesture boundaries are copied after worker tasks finish,
into separate preallocated 4096-entry SPSC rings for renderer and callback.
Per-executor FIFO is preserved; there is no global ordering between these queues
and idle flushes. Output arenas have 256 entries per stage and idle staging has
4096 entries. Overflow drops newest records and is exposed as a cumulative
`dropped` counter, including host flush/arena drops. This bounded transport is
not the future reliable coalescing channel for automation recording. The consumer
must detect loss and resynchronize values and incomplete gestures.

NoteEnd is consumed on the audio side. Held voices match all specified
`(port, channel, key, noteId)` fields; every `-1` is a wildcard, and id `-1`
uses the tuple. Running note IDs differ from compiled IDs so an old loop's end
cannot terminate a retriggered voice. Each note stage reserves 4096 held voices; excess onsets are dropped before DSP
so every onset admitted by the engine has a release mapping;
`voiceDropped` counts overflow cumulatively (offline renders report warnings).
The engine tracks held input notes, not the plug-in's internal release tails.
The host input arena has its own 8192-event cap; recovery for a dropped release
there remains a reported policy/integration issue.
Plan adoption carries running IDs and held addresses, rebinds compiled IDs by
musical source origin/address/duplicate ordinal after MIDI or tempo edits, and
releases removed or no-longer-current onsets. Loop releases use the running addresses. No output transport or voice tracking allocates, frees, locks,
logs or dispatches from the audio path.

The app-side consumer belongs in `EngineApp::tick()` after engine main servicing:

1. Drain the take API and publish cumulative event/voice drop counts in
   `engine.status` and `engine.stats`. On a changed drop count, close incomplete
   gestures and resynchronize parameter values on main; do not invent missing
   automation points. Reject removed FX and superseded plan/transport epochs.
   For anticipative records, use `streamSample`/`playedStream(epoch)` to delay
   timeline recording until that stream position has played (discard abandoned
   seeks). Idle records apply immediately.
2. Validate the parameter against the current instance. Global ParamValue
   records (all address fields `-1`) update `FxInstance::params`; per-note values
   retain their address and do not overwrite a global default. VST3 values are
   normalized: call `Instance::plainParam` on main before storing them. Semantic
   CLAP values also update the matching patch key using core-params' conversion
   to physical units. Preserve flags: `DontRecord` prevents envelope recording,
   while the current value may still be reflected in the model/UI. Outputs for
   read-only parameters (meters, readouts) are display values: they never
   enter the model, history or automation.
3. GestureBegin starts a pending model edit keyed by FX and parameter; values
   update that pending edit; GestureEnd commits one undo version. Do not invoke
   `fx.param.set`/`Instance::setParam` for these values. For semantic patch changes,
   call `InstancePool::acknowledgeOutputPatch` with the updated patch before a
   head-change rebuild, so `get()` does not echo it through `setPatch`. Only
   plug-in-originated parameter changes may use this acknowledgement; other
   edits still follow normal validation and delivery. Plain outputs capture the
   current opaque state and acknowledge its hash through
   `InstancePool::acknowledgeOutputState`; numeric values remain display/diff
   fields. Replay logs retain the resolved value edit and recorded envelope
   points without executing a setter during output consumption. The consumer
   bounds pending records (8192), gestures (128) and recording points per gesture
   (4096); overflow discards missing recording intervals and resynchronizes
   current values. Status exposes `dropped`, `voice_dropped`, and
   `failed_errors` alongside legacy `failed_fx` IDs. Failure entries and
   `fx.failed` retain `reason` and add structured `error` details.
   Known limit: a plain plug-in's state is one opaque blob, so when two of its
   parameters have overlapping gestures (an XY pad), the step committed for the
   first also captures the second's in-progress value.

Bridge integration carries matching additions to the `Op::Service` response in
`host/clap-host/bridge/bridge_main.cpp` and `src/bridge_client.cpp`: carry the
process-request bit and an ordered list of `{param id, clear flags}` after
`paramsRescanFlags`, update both reader and writer together, and expose the
received wake bit to the proxy's RT-safe wake API. Both endpoints use the
unreleased protocol v6 layout (ADR-0013).

### Plug-in catalogue scan reports (2026-10-04)

`plugin.list` and `plugin.scan` return `scan` beside `plugins`, including an
empty catalogue. Its `failures` entries contain `{module, reason: {code, message,
path, hint, details?}, cached}`, `warnings` contains structured reasons, and
`cached_modules` counts reused module results. EngineApp retains this report
with its catalogue so unrelated scans cannot overwrite it. Filtering affects
only available plug-ins; explicit scanning replaces the catalogue/report pair.
