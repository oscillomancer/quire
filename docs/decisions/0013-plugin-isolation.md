# ADR-0013: Plugin process isolation

**Status:** Accepted, 2026-09-26 (dedicated mode implemented; shared bridge next). Spec: `docs/specs/engine.md` §5,
`docs/specs/session-model.md` §1.7 (`FxInstance.isolation`).

## Context

Phase 4 acceptance: a crashing test plugin in an isolation mode does not take
down the engine. The model already carries `isolation: inherit | in_process
| shared_bridge | dedicated` per fx. The engine talks to plugins through
`host::Instance` (clap-host) everywhere: plans, the instance pool, patch
validation, offline renders. With anticipation (ADR-0012) plugins run on the
renderer and worker threads, not in the device callback.

## Decision

1. **Same interface, two backings.** `host::Instance` keeps its public API;
   its implementation moves behind an internal backing interface with two
   implementations: *local* (today's in-process CLAP host) and *bridged* (a
   proxy to an instance in a bridge process). `Instance::create(info, host,
   mode)` picks one. Plans, the pool and the engine do not change.
2. **The bridge executable** (`<slug>-bridge`) hosts plugins with the same
   clap-host library (local backing inside). Dedicated mode: one process per
   instance. Shared-bridge mode: one process for every instance in that mode
   (a crash takes down all of them — the user's choice).
3. **Control plane:** a Unix `socketpair` created at spawn (the child's end
   inherited as a known fd), carrying length-prefixed binary messages for
   the main-thread API (create, activate/deactivate, params, text, semantic
   schema/patch/describe, state, latency/tail, notifications and log
   drain). Requests are synchronous with a timeout; the bridge runs them on
   its main thread and services its plugins' main-thread requests itself.
4. **Data plane:** one `memfd` region per instance, mapped by both sides:
   a header (request/done sequence numbers, frames, flags, result), the
   transport, input and output audio (channels × max frames), an input event
   arena and an output event arena (fixed capacity; overflow dropped and
   counted as today). Lifecycle calls that CLAP puts on the audio thread
   (`start_processing`, `stop_processing`, `reset`, params flush) travel as
   commands in the same header, so they run on the bridge's audio thread in
   order with `process`.
5. **Synchronous processing with a deadline.** The caller (the renderer or a
   worker) writes inputs, bumps the request sequence, wakes the bridge's
   audio thread (futex on a 32-bit word in the shared region), and waits for
   the done sequence — spinning briefly, then a futex wait bounded by a
   deadline (2 s for `process`, 7 s for lifecycle commands: long enough that
   a loaded machine never gets a healthy plugin killed). Output is bit-identical with the
   in-process backing, and PDC is unchanged. (A pipelined mode with one block
   of added latency, for live nodes in the device callback, comes with live
   processing.)
6. **Failure.** The parent watches each bridge with a `pidfd` and the control
   socket. A bridge that dies, or misses the deadline and is then found dead
   or killed, turns its instances *failed*: `process()` outputs silence and
   returns success (the rest of the session keeps playing), `failed()` is
   true with the signal, and the engine reports `fx.failed` on the main
   thread. A hung bridge (alive past the deadline) is killed and failed the
   same way. `fx.restart {fx}` re-creates the instance
   from the model's last known patch/state (the model is the source of
   truth; nothing is lost that was committed).
7. **Test plugin.** The clap-host fixtures gain `com.suppleaudio.test.crash`:
   an effect that dereferences null in `process()` when its `crash`
   parameter is on (and can hang instead, for the deadline path).
8. **Defaults.** `inherit` resolves to the project's default isolation
   (`in_process` until the bridge has shipped a release of testing).

## Consequences

- Each bridged `process()` costs two context switches; the benchmark (B6)
  measures it against in-process for 16 instances.
- GUI embedding across processes (X11 XID to `gui.set_parent`) rides the
  control plane later; parameter automation and state already do.
- Sanitizer presets build the bridge too; the data plane is lock-free on
  both sides except the futex waits.

## Alternatives considered

Pipelined by default (adds a block of latency to every bridged plugin; no
need while the whole graph is anticipative); one shared memory region for a
whole bridge (simpler mapping, but one instance's overflow or crash of the
parser could corrupt another's buffers); sockets for audio (copies and
syscalls per block for the data, not just the wake-up).

## Implementation notes (2026-09-26)

- `host/clap-host/src/bridge_protocol.{h,cpp}` (frames, SCM_RIGHTS, the
  shared-memory Header/Layout, futex helpers), `bridge_client.{h,cpp}`
  (`RemoteInstance`: spawn with `posix_spawn`, pidfd, control requests with
  a timeout, audio-thread commands with a 2 s deadline (lifecycle commands 7 s); kills go through
  `pidfd_send_signal`, so a recycled pid is never signalled; the exit status
  is reaped on the main thread), `bridge/bridge_main.cpp` (`<slug>-bridge`).
  `Instance::create(info, host, isolated)`; every `Instance` method forwards
  when bridged. The one audio-thread syscall exception is scoped with
  `rt::ScopedSanctionedSyscall` (rtsan).
- Latency and tail come back with every `Service` reply (a restart inside
  the bridge may change them without a notification).
- Engine: `InstancePool` creates bridged instances for `dedicated` and (for
  now) `shared_bridge`; replaced or restarted instances are retired and freed
  at the next safe prune. `fx.set {isolation}`, `fx.restart {fx}`,
  `engine.status.failed_fx`, event `fx.failed {fx, reason}`.
- Tests: bridged ≡ in-process (params, text, patches, errors, state,
  latency restart, bit-identical processing with sample-accurate events);
  crash → failed + silent, calls fail with `host.bridge_failed`; hang →
  killed at the deadline; missing bridge → clean error; the Phase 4
  acceptance test (a session plays on while an isolated plugin crashes; only
  its node goes silent); crash → `fx.failed` → `fx.restart` over the bus.
- Not yet: one process for all `shared_bridge` instances, GUI embedding
  across processes, the pipelined mode, B6 overhead benchmark.

