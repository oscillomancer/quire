# ADR-0030: Main-thread service notifications and deadlines

**Status:** Accepted, 2026-10-08. Specs: `engine.md` §6,
`command-bus.md`, `session-model.md` §5.3, `sandbox.md`.
Builds on ADR-0028's renderer waits and ADR-0029's shared relay.

## Context

The app's 100 ms service timer combines queued work with time-based work.
The GUI additionally polls control MIDI every 10 ms. Neither interval is
needed when the app has no queued work or deadline. Removing them requires
identifying every producer, including notifications from plug-ins without
editors and outputs rendered ahead of the listener.

## Decision

EngineApp owns one MainThreadWake, declared before its producers and kept
through all worker joins, backend closes and instance destruction. Producers
publish first and notify afterwards. Audio producers only call the shared
atomic/private-futex relay; they never write an fd. tick() drains and rearms
before consuming queues. Local instances, including persistent probes and
retired instances, subscribe. Every instance receives setMainThreadWake;
remote forwarding is the bridge's contract and belongs to its separate ADR.

Notify for control MIDI batches or drops, plug-in output batches or drops,
plan adoption and ownership release, transport epoch/state transitions,
processing errors, backend status changes, and worker completion. Job events
and posted dispatcher work notify outside their locks. Offline render
progress publishes at most once per 500 ms plus the final block. No notification
is added merely because another unchanged audio block completed.

Render-ahead outputs first wake main for queue ingestion. Main arms an
epoch-tagged stream target for the earliest retained future output. Callback
consumption signals once when it reaches that target. A serial identifies
arms, and sequentially consistent arm/stream operations plus a main-thread
recheck prevent a lost crossing. A new epoch or plan causes main to discard
stale work and rearm. Open transactions retain outputs and dirty-state work;
closing a transaction wakes main to resume them.

The next deadline is the minimum of changed-head autosave (60 s since the
last save/attempt), restart retry (immediate, then 250/500/1000/2000/4000 ms),
restart give-up (30 s), learned plain-parameter rest commit (300 ms), and
local plug-in timers. There is no autosave deadline for an unnamed or unchanged
project, and no learned-commit deadline while a transaction blocks it.
Service timing uses the monotonic clock; history timestamps retain wall time.
The existing fixed clock also drives service timing for deterministic tests.

ModelLoop adds cancellable fd watches and one-shot absolute monotonic timers.
Cancellation on the model thread suppresses even already-ready snapshots;
owners cancel before closing/reusing fds. Headless socket service watches the
app relay and local plug-in descriptors, and arms only the nearest deadline.
Stdio MCP polls those sources with stdin and checks them at frame boundaries,
every 4 KiB of buffered input, and whenever its buffer empties. Partial or
continuous input cannot suppress service. Existing periodic MCP callers and
the blocking istream overload remain compatible. Service flushes job events
before delivering Lua events, so a worker event needs no later command/tick.

For the GUI follow-up, meter producers expose a one-shot activity subscription
with an arm/recheck handshake; active/decaying meters use frame deadlines.
Transport/CPU counters use their existing refresh rate only during playback
or recording. Input/commands arm one sidecar compare/write deadline about a
second later; writer completion posts to main. These GUI loop changes land
separately. Recording disk waits, bounded await helpers and explicit test or
benchmark loops retain their existing bounds. No global safety poll replaces
the app's timers; audio-side safety bounds remain.

## Consequences

A headless app with no pending work or deadline blocks indefinitely in poll.
The GUI can share the same service contract in its next implementation step.
Events remain model-thread-only and no wire schema, render result, dependency,
or audio-thread syscall exception changes. Correctness depends on producer
publication and cancellation ordering, covered by deterministic queue,
watermark, deadline and ready-snapshot tests plus RealtimeSanitizer.

**Implementation note, 2026-10-08:** The GUI follow-up now shares the FD watcher
for app/local plug-in/ModelLoop sources and uses the panel/activity deadlines
specified above (gui.md §1). UI input coalesces into the earliest sidecar check.
The activity subscriber supplies its display floor to avoid sub-floor noise
waking stationary meters. No decision or bridge contract changes.
