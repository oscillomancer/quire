# Spec: the engine

**Status:** Implemented; amended with each change (dated sections and ADRs).
**Decision records:** ADR-0004 (backends), ADR-0008 (model/plan
separation), ARCHITECTURE §3 (threads, channels, RT rules, main-thread
budget), §6 (bridging, latency changes).
**Code:** `host/engine` (`supple::engine`), with plugin hosting and the
bridge process in `host/clap-host`.

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
  containers compiled into a small per-chain program. As built: containers
  are not implemented. A stage with `parallel` runs beside the one before it
  on the same input and the branches' outputs are summed (each branch is
  what that plug-in would produce in series, so a bypassed branch adds its
  dry input); pinned stages and strips with auxiliary channels run in
  series, with a warning. `oversampling` 2/4/8 runs the plug-in at that
  multiple of the plan rate behind `dsp::Oversampler` half-band resamplers
  on its main pair and key (not with auxiliary outputs: those run at the
  plan rate, with a warning); event times scale by the factor.
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
  monitoring chains): its own wet/dry and pin delays still match it, but
  its chain reports none of its latency, so its track arrives late.
- An oversampled stage's latency is the resamplers' round trip (72/80/83
  samples at 2/4/8x; an instrument's output crosses only the downward half)
  plus the plug-in's own latency at the high rate, padded at the high rate to
  a whole number of plan samples, so compensation stays exact.
- Input monitoring paths are not delayed to align with playback (they are
  live); recorded material is offset by the backend's reported input latency
  plus the monitored chain's latency so it lands where it was played.
- Input FX (a channel's `input_chain`) process its record input whenever
  the channel is armed with an audio input, monitored or not: such a channel
  is live (the device input exists only in the callback). Monitoring hears
  the processed input, and the recorder records it, dropping the input
  chain's latency from the file's start so the take lines up with what was
  played (the file ends with that many frames of silence). Input FX are
  outside compensation and play no part in offline renders.
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
  one version per gesture. Capture timestamps are seconds; evaluation, entry
  guards, erase windows and stop holds convert them into each envelope's
  stored domain (seconds for `time`, tempo-map beats otherwise). Plugin-output
  recording follows the same conversion, and replay carries time-envelope
  points as explicit seconds through `env.points.set`.

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

A node is **live** when it consumes monitored hardware audio/MIDI or armed
audio input (including an input FX chain). Live classification propagates
downstream, including sends and the master. All other nodes are anticipative.
Opening a plug-in GUI or MIDI editor does not currently change classification;
editor/automation eligibility preferences remain planned (ADR-0014).

- **Anticipative nodes** run on the renderer and background worker pool in
  larger blocks (default 1024 samples), up to **200 ms** ahead. A nonsplit
  graph publishes its master into an SPSC ring; a split graph publishes each
  renderer-to-live frontier edge into a ring. The callback consumes those
  buffers at device block size. Parameter edits can take effect at that
  horizon. A seek/structural edit establishes a new epoch and prerolls;
  starvation after priming counts as an anticipation miss.
  **Renderer waits (ADR-0028):** while stopped, its share processes at the
  next render-block deadline. A process/flush request or queued parameter edit
  can bring that deadline forward, to at least 1 ms after the previous stopped
  block finishes. A notification by itself does not cause an extra block.
  While playing, the callback wakes it once enough audio has been consumed
  for another block. A full master/frontier ring must first release a chunk.
  Missed-wake insurance is half the effective render-ahead span during
  playback (normally 100 ms). It is 1 s while stopped, held, planless or all
  live. Plan and transport changes, hold/release, shutdown, callback epoch
  acknowledgement and instance ownership release also wake the renderer.
  Bridged requests arrive through existing main-thread service.

  `rt::waitUntil(word, seen, deadline)` waits off the callback until the
  sequence changes or its steady-clock deadline expires. It rechecks after
  interruptions and returns true on change, false on expiry.
  `Instance::setAudioWake(word*)` subscribes on main; null detaches and waits
  for outstanding notifications before the word can be freed. Plan retirement
  and engine stop detach subscriptions. `processRequested()` also reports
  queued active VST3/bridged `setParam` edits; `takeProcessRequest()` consumes
  them together with any CLAP request.
- **Live nodes** run on the callback and its separate bounded live pool
  (ADR-0024). With anticipation disabled this executor runs the entire graph.
  Limit 1 processes it serially. Synchronous
  bridged stages pass through on live nodes, with a warning (§5).

### 2.2 Worker pool

The background/offline pool has `max(0, cpus − 3)` helper threads by default,
where `cpus` counts the CPUs in the calling thread's affinity mask (taskset,
cpusets), not every online core; its submitting thread also executes tasks. `EngineOptions::workers` can
override the helper count. A bounded MPMC ready queue and atomic dependency
counters distribute nodes; input summation stays in fixed graph order.
ADR-0023 bounds each run to the smaller of the configured capacity and a
conservative graph-width bound: a greedy disjoint dependency-chain cover.
Only selected helpers are notified through individual sequence slots. Chains
run serially on the submitter without queue operations. During parallel jobs,
participants park when no work is ready; successor publication and final
completion advance an atomic event, observed before checking either predicate
to prevent missed wakes. There is no application spin/yield loop. Enrollment
precedes publication, and the submitter waits for every selected helper's
departure before reusing job state or returning, including preempted helpers.

`EngineApp::setAudioThreadLimit` sets the app default (0: Automatic, 1..64:
maximum execution threads per pool including its submitting thread). Invalid values return
`engine.audio_threads`, path `/audio_threads`, with a corrective hint. The GUI
persists this as `audio_threads` in preferences (§6 of gui.md). The next engine
start, including project replacement and device recovery, maps a nonzero limit to `workers = limit - 1` when
options request automatic workers; explicit `EngineOptions::workers` takes
precedence. Both live and background pools use this capacity independently; it is not a
total process thread limit. Already running pools and offline export's
`threads` option are unchanged. Automatic retains the CPU-mask-derived capacity and graph-aware
participation; an explicit limit is also graph-bounded.

DSP tasks are RT-safe; ADR-0013 separately permits synchronous bridge futex
wake/wait with a failure deadline on background threads, never in the device
callback.

**Scheduling (ADR-0026).** The backend supplies the device callback's
scheduling, and the engine never changes it. The engine derives the other
classes from it, all process-local:

| Class | Policy | PipeWire default | Rule |
|---|---|---|---|
| Device callback | the backend's | FIFO 83 | never changed |
| Live helpers | FIFO | 83 | the callback's priority (ADR-0024) |
| Renderer, background helpers, bridge audio threads | FIFO | 10 | `Engine::kBackgroundPriority`, lowered to the callback's − 1 and to `RLIMIT_RTPRIO` |
| Offline renders, recording writer, main/GUI, jobs | normal | – | unchanged |

- **Why 10.** It is below the kernel's threaded interrupts and display
  workers (FIFO 50) and the audio threads rtkit grants other applications
  (up to 20). Any real-time level outruns normal desktop load, and the
  200 ms render-ahead needs no more than that.
- **When the background classes are real-time.** Only when the callback is
  FIFO/RR (`Backend::callbackPolicy()`; JACK reads its client thread). A
  normal callback (the dummy backend, a non-RT JACK server) leaves them
  normal. `EngineOptions::background` forces `Normal` or `Fifo`
  (`backgroundPriority`) for benchmarks and tests.
- **How it is applied.** On the main thread after backend start, through
  `pthread_setschedparam`, verified by reading the policy back
  (`setThreadPolicy`, core-base `supple/base/thread.h`). JACK's and rtkit's
  thread APIs are not used here. It is all or nothing: the renderer and every
  helper stay normal, with a `warnings` entry naming the cause and the fix, when
  - no priority ≥ 1 remains;
  - `RLIMIT_RTTIME` is finite (a renderer catching up runs without blocking
    and would be killed);
  - the renderer plus its helpers would leave fewer than two of the process's
    allowed CPUs to the rest of the system;
  - or any thread refuses.
- **Lifetime.** Threads end at `stop()`, so nothing is restored. CPU affinity
  is never changed: threads keep the inherited mask.
- **Bridges.** `host::setBridgeAudioPolicy` carries the applied background
  policy to bridged instances while the engine runs (normal otherwise): each
  bridge audio thread takes it when its processing starts (§5).
- **Reporting.** `Engine::scheduling()` records each class's applied policy
  at start (None for a class without threads, and for all while stopped).
  `engine.start` and `engine.status` report it as `scheduling`: `callback`,
  `live`, `renderer` and `background` objects, each with `policy` (`none`,
  `normal`, `fifo`, `rr`) and `priority` (0..99).

**Thread names.** Every audio thread the host creates names itself when it
starts (`nameThread`, core-base `supple/base/thread.h`; Linux keeps 15 bytes),
so `ps -L`, `top -H`, debuggers and profilers tell them apart. The names carry
the role, never the product: `audio-render` (renderer), `audio-bg-N`
(background/offline pool helper N, from 1), `audio-live-N` (live helper N),
`audio-bridge` (a bridge process's per-instance audio thread), `audio-dummy`
(the dummy backend's thread), `audio-recorder` (recording writer),
`audio-offline` (offline render thread, engine and sandbox) and `audio-paced`
(the benchmark's paced device). The JACK callback thread belongs to the JACK
library (PipeWire's `data-loop.N`) and keeps its name. `pw-top` lists graph
nodes, not threads; it shows the JACK client name.

**Live pool (ADR-0024).** `LiveWorkers` shares the graph/ready-queue
representation, but never calls the blocking background executor. Each segment
publishes its immutable transport, device inputs and frontier buffers, opens
the job, then advances selected helpers' private 32-bit sequences and calls
`rt::wake`. This core-base API issues only `FUTEX_WAKE_PRIVATE` under the scoped
RTSan exception; its blocking `rt::wait` companion belongs only to idle helpers
and rechecks the acquire-loaded word after spurious wakes/EINTR. Both require a
live, naturally aligned lock-free 32-bit atomic and one waiter per word. C++
atomics publish the job; futex calls convey no memory order.

The plain job pointer/function/context are published while closed, after the
previous join has excluded all readers. Opening is a seq_cst store. Each waking
helper increments active (seq_cst), then reads open (seq_cst): only if open may
it read job fields and help; otherwise it touches no job state. It decrements
active with release after all accesses. When all nodes finish, the callback
closes open (seq_cst) and spins on active (seq_cst) until zero. This store-load
handshake needs seq_cst on both sides: either the callback observes the claim
or the helper observes closed. A helper preempted before claiming does not
retain the job; an old wake may harmlessly claim the next open job.

The callback helps execute ready nodes and spins on atomics (CPU relax only)
while work or claimed departures remain. Helpers spin only within that job and
park between segments, with no post-cycle spin. Participation uses the chain
cover, configured capacity and usable CPUs minus one; a chain runs directly
without queue/wake traffic. The callback joins a helper preempted after claiming,
including after completing DSP. No helper touches the job after its departure
acknowledgement. Only after join may the callback publish
meters/plug-in outputs, capture recording, reset/relocate, adopt a plan, or
acknowledge a newer generation. This extends existing plan reclamation and
ownership tokens to every helper. There is no late-output discard, timeout or
rerun: a late in-flight node makes its cycle late. A processing error still joins
all work before silencing/counting the failed segment; it never cancels in-flight
plug-in state. Summation order and numeric policy (§2.4) remain identical to
offline processing. Synchronous bridge stages still pass through, including on
helpers.

`Backend::liveThreadCapacity()` and `prepareLiveThread(native_handle)` run on
main after backend start, before publishing the pool to callbacks. Unknown
backends refuse setup by default; normal/dummy backends explicitly accept
normal helpers. JACK reads the actual callback thread's policy/priority and
allowed CPU set through its optional client-thread API and pthread queries.
Live capacity is capped to CPUs common to callback and caller minus one.
SCHED_FIFO/RR callbacks require helpers at the same priority with SCHED_FIFO:
try `jack_acquire_real_time_scheduling`, then pthread scheduling within
RLIMIT_RTPRIO, and verify the result. Non-RT JACK uses normal helpers. Failed
setup/capacity disables the pool and retains serial processing with an
actionable warning. No affinity or system configuration is changed; externally
changing CPU affinity while running requires an engine restart. The backend
stops before helpers are destroyed. `Engine::liveHelpers()` reports enabled
capacity; `engine.start`/`engine.status` expose `live_helpers` and `warnings`
(including plan and scheduling warnings), zero/empty when stopped apart from
optional process-control failures described below. These are
capacity, not a count of participants in every segment. Automatic enables this
path; Audio processing threads Limit 1 disables it on next start.

### 2.3 The RT cycle

1. Pick up a new plan if published (acknowledge epoch).
2. Drain the model→RT command queue (transport, param events, meters on/off).
3. Read hardware inputs into HwInput buffers; feed record rings.
4. Pull anticipative outputs from rings; fork/join the live DAG.
5. Sum into the master, run the master chain (live if anything is live, else
   the master is anticipative too and the RT thread only copies its ring
   output).
6. Write hardware outputs; publish meters (lossy rings) and state deltas
   (coalescing slots).

**Meters**. Per strip, the peak and the mean square of the strip's signal
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

### 2.4 Floating-point environment and first touch (ADR-0025)

`detail::processNode` scopes `dsp::DenormalGuard` around each node: its
input sums, delays, Console 9 stages, faders, oversampling, meters and every
in-process plug-in `process()` run with FTZ and DAZ added to the executing
thread's MXCSR, which is restored when the node returns. Every executor runs
nodes only through `processNode`: the device callback (serial or with live
helpers), the live helpers, the anticipative renderer, background workers and
offline renders. Each node therefore computes under the same environment in
real time and offline, whatever thread runs it, and the device callback's
thread (owned by JACK/PipeWire) leaves every node with its own MXCSR. A
plug-in that changes MXCSR in `process()` affects at most the rest of its node.
The bridge applies the same flags around each bridged `process()` (§5), the
offline stem decoder around its decode (§3), and the plug-in sandbox around
each render block.

Plug-in lifecycle calls (`start/stop_processing`, `reset`, parameter flush
outside `process()`), transport computation, copies, the metronome and
offline post-processing (normalization, limiter, dither, encoding) keep the
thread's environment in every executor. A value that underflows inside a node
becomes zero; normal-range results are unchanged. The guard is SSE2-only;
other architectures keep their default environment.

Audio sample buffers (node and stage buffers, delays, the render-ahead and
frontier rings) are zero-filled on the main thread when a plan or ring is
built, which commits their pages before any callback uses them. Owned audio
threads (live helpers, background workers, including an offline render's, the
renderer and the bridge's audio threads) call `rt::prefaultStack()` once
before their first job: it touches the 128 KiB of stack below the caller and
does nothing when less than 512 KiB remains. The device callback's thread is
not ours and is left alone. Reserved event and voice capacity is committed on
first use, and no plug-in code runs early to warm it.

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
  master-less path, or through master using an exclusive solo — option), regions,
  the render matrix, selected items; tail handling (`tail_ms` or until
  silence); normalization (loudness via core-analysis, then gain, then
  optional true-peak limiter — deterministic, two-pass); dither (seeded
  TPDF); formats WAV (PCM/float, RF64 beyond 4 GiB) and FLAC (the original
  audio-file encoder); file
  naming per session-model §7.3; a job
  queue; results reported as files + analysis summaries.
- A post-fader stem of a channel or buss is decoded outside the graph, under
  the same floating-point environment as the graph's nodes (§2.4).
- Through-master stems use playback's solo signal-path rules: retain the
  selected strip, its upstream contributors and its path through parents and
  ordinary sends to the master. Ignore other pre-existing solos and solo-defeat;
  preserve explicit mutes. Sidechain keys retain playback's dedicated taps:
  key sources continue driving FX even when their audible output is solo-
  silenced; post-fader keys still follow the source fader, pan and mute.
- Normalization rejects non-finite source samples, derived gains, or resulting
  float samples with `render.normalize` and a repair hint before applying gain.
  This is a representability check, with no additional dB target range policy.

## 4. Media, recording, device handling

- **Media cache**: decoded and resampled audio (Signalsmith-quality
  resampler or the audio-file polyphase resampler), in memory and keyed by
  media identity, engine rate and playback variant (implementation notes below).
  Disk backing under `$XDG_CACHE_HOME/<slug>/media/` and streaming readers
  prefetching on I/O threads into per-item rings (default 1200 ms like REAPER's
  media buffer) remain planned.
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
  ordered loop runs beginning at file frame zero, with the loop range they were
  recorded in, or `[0, 0]` when seeks split the runs while no loop was active
  (an inactive loop range is never recorded); invalid sidecars return a
  structured error and must be skipped during recovery. Placement sidecars
  survive pending hash jobs (including finalized loop runs at stop); the
  background finalization adds an optional `hash` without changing their
  recovery contract.
- **Backends** (ADR-0004): JACK (dlopen), dummy, offline in P4; buffer-size,
  sample-rate and device changes stop the engine cleanly, rebuild the plan,
  and restart; xruns are counted (backend callbacks + our own deadline
  misses) and emitted as `engine.xrun` events with context (block size,
  load, which node was slowest).

## 5. Plugin isolation (`host/clap-host/bridge`)

Modes per instance (session-model §1.7): `in_process`, `shared_bridge` (one
bridge process for all instances in that mode), `dedicated` (one process per
instance).

- The bridge executable (`<slug>-bridge`) hosts plugins with the same
  `clap-host` library. Control plane: a Unix socket speaking a small binary
  protocol for main-thread calls (activate, params, state, semantic
  extension, GUI create/set_parent with the host's X11 XID, timers).
  Data plane: a `memfd` shared-memory region per instance with input/output
  audio buffers, event arenas and a cycle header; wake via eventfd/futex
  through the bridge's shared futex wrapper (ADR-0013, distinct from the live
  pool's private `rt::wake`). The shared memory is laid out for the widths the
  host asks for; the Activate reply (protocol version 9) returns the widths
  the plug-in settled on (a VST3 may adapt its buses), and a mismatch
  re-activates once at those widths, so an isolated plug-in exposes the same
  layout as an in-process one.
- The bridge main loop polls its control socket, a coalesced main-thread
  request eventfd and every registered CLAP/VST3 descriptor, even without an
  open editor. Its timeout is the nearest plug-in timer deadline, rounded up
  to milliseconds (0 when due), or -1 when there are no timers; no fixed idle
  cap remains. A parked relay translates audio-safe atomic/`rt::wake`
  notifications into eventfd writes (`supple::MainThreadWake` in core-base;
  core-base spec, ADR-0029; subscription in sandbox spec §1). It drains/rearms
  the wake before servicing every instance, before each poll, so neither
  concurrent requests nor a busy control socket starve main-thread work.
  Parent death is the control socket's HUP/EOF, including when no instances
  remain. The wire protocol is unchanged; notifications and editor requests
  still reach the host through its existing control requests.
- The bridge's audio thread sets FTZ and DAZ around each `process()` and
  restores its MXCSR afterwards (§2.4), so a bridged plug-in computes as an
  in-process one does.
- Each instance's audio thread is named `audio-bridge`. On `StartProcessing`
  it takes the scheduling the host writes into the shared-memory header
  (`audioPolicy`, `audioPriority`; protocol version 10), which is the engine's
  background policy while an engine runs and normal otherwise (§2.2,
  ADR-0026). A renderer waiting for a bridged plug-in therefore never waits
  behind a lower-priority thread. Applying it is best effort: a refusal leaves
  the thread as it was.
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
bus and exposes it through the control protocol (Unix socket), Lua and MCP (proxy or in-process). Engine-specific commands:
`engine.start {backend, device, rate, block}`, `engine.stop`,
`engine.status` (q: backend, rate, block, load, xruns, anticipation
misses, meter drops), `engine.graph` (q: nodes, live/anticipative classification,
latencies, CPU per node), `engine.settings.set {render_ahead_ms,
worker_threads, automation_resolution, …}`.
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

The tick also polls the devices' **control messages** for MIDI learn
(ADR-0027): each callback copies its notes, CCs, program changes and pitch
bends into a 1024-entry SPSC ring (`Engine::takeControlMidi`; overflow
counted, `controlMidiDrops`), and `EngineApp::pollControl` drains it, hands
the messages to `onControl` (the window app's action bindings and learn
dialog) and moves the plug-in parameters learned for them (fx `learn`): a
semantic parameter through `fx.param.set` with one gesture per movement, a
plain one on the running instance at once and into the project when the
control has rested for 300 ms (as an edit in the plug-in's own editor).
Nothing moves while `setLearning(true)` (a learn dialog listens); bindings
with `when_*` conditions ask `learnGate` and wait without one. The window app
also polls every 10 ms while anything is bound. `lastTouched()` is the
parameter edited last from the window or a plug-in's editor (not by agents or
controls); `moveParam` moves any parameter the way a binding does, for the
"last touched parameter" action.

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
- **B6 isolation:** crash test (§5) + CPU overhead of synchronous background bridge vs
  in-process for 16 instances.

Results are recorded with kernel, PipeWire version, quantum actually
granted, and CPU governor (read-only facts).

B1/B2 tooling: [tools/bench/README.md](../../tools/bench/README.md).
The command-built shared session, paced search and REAPER offline generator
are dry-verifiable. Real measurements require `SUPPLE_AUDIO_DEVICE_TEST=1`
exactly. Historical device results are in `docs/benchmarks/b1.md` and
`b2.md`; their recorded revision, settings and duration bound the claims. The
search retains every trial, independently confirms its candidate, and labels
a passing search cap as a lower bound. REAPER counts observed audio/media
underrun timestamp changes (multiple events between polls can be undercounted).

### 7.1 Benchmark-only measurements

`EngineOptions::benchProbe` and `JackOptions::benchProbe` default to null.
Only `engine-bench` enables these pointers; there is no GUI preference.
`BenchProbe` owns preallocated, bounded single-producer sample arrays, one for
the backend and one for the renderer. Allocate it before start, retain it until
both stop, and inspect it only after producers join. One measurement interval
per probe; an atomic gate excludes preroll. Newest samples are dropped and
counted on overflow; a benchmark with dropped samples is invalid. No sorting,
allocation, formatting or logging occurs on a producer. Disabled probes read
no clock and allocate no engine storage.

The JACK timer surrounds the whole process callback, including all engine
chunks and backend bookkeeping in a granted period. Paced timing surrounds
one synthetic callback. Benchmark input is silence and physical outputs are
silenced after measuring the pre-output peak. Render-job timing surrounds
playing `processRendererBlock`, including pool join, excluding ring publication
and idle/pacing waits. Nearest-rank p50/p99/p99.9/max and mean are in microseconds;
load divides total time by the captured frame duration. Timing start/end reads
add measurement overhead. A sample begun before the gate closes may finish
after it; aggregate status snapshots can differ by one boundary cycle.

Ring low-water is a callback observation before consumption, after priming in
the current epoch: published renderer stream frames minus consumed stream
frames, clamped to zero. Publication of the stream counter can lag ring
publication; this is a conservative observed ahead distance, not an atomic
snapshot of every frontier ring or an assertion about minimum wake latency.
It is null when there is no sampled anticipative boundary (including an
all-live graph); observations and anticipation misses are reported separately.

`tools/bench` accepts 64/128-frame requests. Run JSON is
`supple.capacity-run/2.1`: `xruns` is a JACK server counter delta or null for
paced runs; `live_helpers` reports enabled live capacity (0: serial);
`synthetic_over_budget` and `synthetic_late_wakes` are paced-only
counters (null for JACK). Callback `over_budget` is a duration test, never a
hardware xrun count. Requested block, granted quantum/rate and
`requested_settings_granted` are separate. Mismatched grants remain useful
fixed-run data but cannot pass a requested-setting capacity search. A model
tick that retires the engine after device loss aborts the run with
`bench.device_changed`, cancels automatic restart and never samples the retired
engine. No forced quantum or rate is applied. Optional third-party effect
fixtures take an
installed module path and descriptor ID; binaries are never vendored by the
harness. See the tooling README for schemas, limits and invocation details.

## Implementation notes (2026-09-26)

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
  The granted rate is the engine's rate, read once the client has run its
  first cycle (at most 1 s after activation): before that pipewire-jack
  reports its default rate or the latency request's denominator, and its
  rate callback flips while the client joins (a graph a browser started at
  44.1 kHz reads as 48 kHz). Only a later reported rate that differs from
  it is a rate change. A project at another rate plays at the device's
  (positions are in seconds, media is resampled to the plan's rate, plugins
  are activated at it; renders stay at the project's rate). Stop:
  the next process cycle makes the final zero-frame callback; if the server
  is gone or no cycle comes within 2 s, the client is deactivated and closed
  and the engine stops its plugins on the main thread. Xruns (server
  callback), rate changes and server shutdown are atomics read by
  `engine.status` (`restart_needed`); the model thread's service tick (§6)
  restarts the engine after a device change. Tests use a stand-in libjack
  (`host/engine/tests/fake_jack.cpp`) that drives the
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
  after the audio already rendered, no gap). Renderer parking follows
  ADR-0028: stopped block deadlines and event-driven buffer-space wakes
  from the callback, with a safety bound of half the playback buffer span
  (1 s while stopped, held or all live). Misses (no chunk while playing, after
  the pre-roll) are counted (`engine.status.anticipation_misses`). `stop()`
  joins the renderer first; it stops every plugin as it ends. With
  `anticipative = false` the old in-callback renderer runs (the reference,
  and the base for live nodes).
- **Worker pool** (`workers.{h,cpp}`): helper threads (every allowed CPU but three,
  plus the submitting thread) run one render block as a DAG over the plan's
  nodes: per-node atomic counts of unfinished sources, a bounded Vyukov MPMC
  ready queue, the submitter helps; graph-bounded participation and idle
  parking follow ADR-0023. The submitter joins every enrolled helper.
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
  "800 Hz" for plugins). Write/touch/latch recording uses the envelope's own
  timebase, including points received from plugin output events (§1.4).
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
- **Media resampling:** `MediaCache::get(path, rate, variant, contentHash)`
  decodes and resamples once per (media identity, rate, variant) with the
  audio-file polyphase resampler (Kaiser
  windowed sinc; deterministic, so offline ≡ real time); plans ask for their
  own rate. The real-time engine compiles at the device's rate, so a project
  at another rate plays (previously refused). The cache is in memory and the
  resampling is synchronous at plan compile. A supplied `MediaSource.hash`
  identifies both decoded audio and its variants, independent of pathname.
  Without a hash, identity is device, inode, size and nanosecond mtime,
  inspected on the model/decode thread at preparation (and checked again
  after decode). A changed identity gets new immutable buffers; old plans
  keep their leased buffers. Decoded originals are weakly indexed: they live
  only while a plan or passthrough variant leases them; expired entries are
  pruned on variant misses. Additional variants may decode the original again
  after its last lease ends. Resampling and stretching read immutable input
  directly, without a preliminary full-buffer copy. Disk backing and background
  decode remain planned.
- **Take playrate and pitch:** a take's playback variant is built once per
  (media identity, plan rate, variant) in the same cache: varispeed resamples to
  `rate / speed` (`pitch_mode: "resample"` or `preserve_pitch: false`), the
  default `"signalsmith"` mode stretches with Signalsmith Stretch (MIT,
  seeded, so deterministic) and transposes by `pitch`. The stretch runs at
  plan compile on the main thread (~0.3 s per source second in a debug
  build); it moves to the background decode with the disk cache.
  `stretch_markers` are not applied yet (the compiler warns).
- **Live pool** (`live_workers.{h,cpp}`): callback fork-join as specified in §2.2.
- **Not yet:** MIDI output ports, crossfades for
  structural changes (inserting or removing plugins). B1/B2 measurements:
  `docs/benchmarks/b1.md`, `b2.md` (tooling: `tools/bench/README.md`).

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
plan each cycle, the renderer's share at render-block pace (a request can
run early, at least 1 ms after the previous stopped block finishes); play
re-locates the non-live nodes and resets their plugins.
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
`plugin.scan {wait: false}` returns at once with the current catalogue and a
`job`: a worker scans every module through the supervised scanner (stock ones
too, since nothing may load in that thread; without the bridge the job fails
with `host.scan.unsupervised`), and the model thread installs the new pair
when the job is done. A background scan still running is returned instead of
a second one.

### Initial engine-start failure (2026-10-08)

A failed initial plan load stops and releases the engine/backend before
`EngineApp::startEngine` returns its error, including with a caller-supplied
backend. A corrected session can start again without an intervening stop.


## Optional host-process controls (2026-10-08)

`EngineApp::setMemoryLock(bool)` changes an app-wide, default-off request.
Loaded Preferences apply before startup; setters apply immediately on the
model thread while running or store intent while stopped. After a successful
backend start, numeric validation and initial plan load, the app acquires its
requested controls. Initial allocation/setup is not protected. An acquisition
failure warns but does not prevent usable audio from starting. There is no
retry loop; the next successful start or explicit re-enable retries.
`haltEngine()` stops the backend and processing workers before releasing the
controls. Project replacement/device recovery release and reapply them.
Transport stop/pause keeps them: the engine still processes monitored inputs.
The helper destructor also releases acquired controls. No control operation
runs on an audio path, and offline rendering acquires no separate request.

### Memory locking

Use `mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)` with no fallback.
CURRENT covers existing mappings and FUTURE covers later in-process plug-ins,
sample libraries and heap/stack mappings. ONFAULT locks resident pages and
marks untouched pages for locking when populated instead of committing
untouched stacks/arenas. It does not prefault memory or eliminate cold-page
faults. The request covers the entire host process, including GUI/media and
in-process plug-ins; shared/dedicated bridges and scanner processes are
excluded. Host-side shared mappings do not protect bridge heaps/stacks.

Read `RLIMIT_MEMLOCK` for failure diagnostics without changing it. ENOMEM and
EPERM include the account's allowance and suggest disabling the option,
reducing mapped memory or asking an administrator to review the allowance;
EINVAL reports unsupported flags. Unlimited allowance is not unlimited RAM.
Future allocation or stack expansion can fail under the policy; stack failure
can terminate the process. Large sample libraries may fail to load.

Disabling/stopping calls `munlockall` only after our successful acquisition.
It also removes locks made by in-process plug-ins/libraries. Locks do not
stack, and prior independent lock ownership cannot be restored: opting in
accepts this process-wide cleanup behavior. Default-off and failed acquisition
never unlock another plug-in's locks. A failed unlock retains active ownership
and its error for a later cleanup attempt. In-process third-party locking
calls can alter the policy; status reports our last successful operation.

### Status and test seam

`engine.start`/`engine.status` add `memory_lock`: `{requested, active, error?}`.
The optional typed error has `code`, `message`, `path`, `hint`, and numeric
`errno`. Codes are `engine.memory_lock`/`engine.memory_unlock`, path
`/lock_memory`. Intent and cleanup errors remain visible while stopped;
requested=true/active=false without an error means pending while stopped.
Failures also appear in `warnings` alongside existing engine warnings.
AppOptions may inject `RuntimeProtectionOps` for deterministic lifecycle/error
coverage. Default wrappers call Linux libc; these operations are model-thread
only. Tests fake memory locking instead of locking the runner's address space.


### CPU idle latency (2026-10-08)

`EngineApp::setPreventDeepIdle(bool)` changes the separate default-off
request. Open `/dev/cpu_dma_latency` with `O_WRONLY | O_CLOEXEC`, write one
native `int32_t` of `kCpuIdleLatencyUs = 20` microseconds and retain the fd
while enabled and the engine runs. C1-class exits are typically a few µs;
deep states take roughly 100 µs to over 1 ms, a substantial part of a
64-frame period. This fixed small threshold permits shallow idle instead
of the zero-latency POLL-only constraint on the measured workstation.
There is no runtime sysfs/topology read to choose the value, frequency lock
or guarantee of a particular C-state residency on arbitrary hardware.

The request affects system idle while held and may increase power, heat and
fan activity. Quire offers this owned, close-scoped QoS request instead of
locking CPU frequency: governor/EPP changes are privileged and system-wide.
The request leaves frequency policy alone and never escalates privileges.

Retry EINTR on open/write; a zero/short write fails with EIO and is never
completed with partial integer writes. Every failed write closes its fd.
Disable, stop, project/device restart and destruction close the acquired
fd exactly once. Closing removes only this request, without reading or
restoring a remembered global QoS value. On Linux close is not retried even
on EINTR: the descriptor is already released and may have been reused.
CLOEXEC excludes child bridge/scanner processes from retaining the fd.

`engine.start`/`engine.status` add `cpu_idle_latency` with the same typed
`{requested, active, error?}` structure as memory_lock. Failures use
`engine.cpu_idle_latency`, path `/prevent_deep_idle`, with numeric errno and
a corrective hint. EACCES/EPERM explain that the account needs membership
in a group with device write access (commonly audio; policy varies);
ENOENT/ENODEV report that the device is unavailable. Acquisition failure
keeps intent enabled and audio usable; close failure records an error with
active=false because the descriptor has been released.

Tests inject open/write/close operations for all lifecycle and error paths.
The real libc write/close path is exercised against a temporary file through
an injected open. Automated validation never writes to the real device.
User-only acceptance: enable/disable and engine-stop/restart/exit cleanup
with the real device, plus repeated sparse/dense 64/128-frame sessions,
actual granted period, callback tails/xruns and power/thermal/fan behavior.
No latency benefit is claimed without those measurements.
