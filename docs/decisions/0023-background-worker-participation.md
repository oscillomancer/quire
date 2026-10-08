# ADR-0023: Bound background worker participation and park idle participants

**Status:** Accepted, 2026-10-07. Amends ADR-0012 §6 for background/offline
scheduling. Spec: `docs/specs/engine.md` §2.2.

## Context

The pool woke every helper for every block. Once its ready queue emptied,
idle participants repeatedly yielded until the entire graph finished. Four
independent expensive effects used about 2.5 CPU cores with seven helpers,
versus 1.25 with three, despite needing only four execution threads. Reducing
the pool globally would sacrifice wide-graph throughput. Graph/job lifetime
must remain protected even when a helper is preempted before joining.

## Decision

- Keep the configured pool capacity (automatic: cores minus three helpers
  for anticipation; offline retains its existing thread setting). At graph
  construction, greedily cover nodes with disjoint chains along dependency
  edges. The number of chains is a conservative upper bound on simultaneous
  useful tasks, not an exact maximum-width calculation. Unequal-depth
  branches must not be limited by the size of the largest topological level.
- For each run select `min(pool helpers, chain count - 1)` helpers. Only
  their individual native futex-sized (32-bit) sequence slots are published/
  notified, avoiding shared wait-table wakeups for larger atomic values on
  Linux. A selected slot must acknowledge before advancing again, so sequence
  wrap cannot be missed by a waiter. A serial graph,
  or a pool with no helpers, executes topological indices on the submitter
  without queue/dependency operations or waking helpers.
- Enroll all selected helpers before publishing the job. The submitter also
  processes tasks, then waits until every enrolled helper has acknowledged
  departure. A helper delayed before joining still owns this job's lifetime;
  it cannot read a replacement graph or freed context. Unselected helpers
  never read job pointers. Publication uses release/acquire ordering and
  departure uses an acquire/release countdown.
- Participants with no ready work park on a monotonically increasing atomic
  event. Read that event before inspecting completion and popping a task.
  Publish successors before changing/notifying the event; final completion
  also changes/notifies it. This covers publication between observation and
  wait, including a reserved but not yet published MPMC queue cell. Keep
  dependency release/acquire ordering and fixed input summation.
- Park the submitter while waiting for helper departure, too. There is no
  application spin budget or yield loop. This scheduling code is expressly
  background/offline only; DSP tasks retain their RT annotations and rules.
  The device callback remains serial and never calls this pool.
- Provide an advanced app preference, **Audio processing threads**, default
  Automatic; explicit limits count the renderer plus helpers. It applies on
  the next engine start, including project replacement and device recovery. It does not resize a
  running pool or change offline export's separate thread option. Explicit
  `EngineOptions::workers` still overrides the app preference.

## Consequences

Narrow graphs avoid unnecessary participation and idle participants stop
consuming CPU. Wide graphs retain the existing capacity. A chain cover can
be conservative for a complex DAG, and every selected helper must wake even
if the submitter finishes first; this trades possible dispatch latency for
simple, explicit lifetime ownership. Pool threads remain allocated while
inactive. No priority, affinity, render-ahead pacing, DSP, bridge protocol,
or device scheduling policy changes here.

Deterministic tests pause helpers at join, departure, and before waiting;
reclaim/replace graphs immediately; exercise a serial prefix followed by a
fan-out; and check thread-count-independent renders. Performance acceptance
also requires paired job/callback tails and ahead headroom, not CPU alone.
