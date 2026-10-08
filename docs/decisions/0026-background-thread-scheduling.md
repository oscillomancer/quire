# ADR-0026: Background audio thread scheduling

**Status:** Accepted, 2026-10-08. Amends [ADR-0012](0012-anticipative-processing.md)
(the renderer and pool ran at normal priority), [ADR-0023](0023-background-worker-participation.md)
("no priority changes") and [ADR-0013](0013-plugin-isolation.md) (the bridge
audio thread's scheduling; protocol version 10).

## Context

The renderer renders up to 200 ms ahead of the device callback. It works
with the background pool's helpers, and calls bridged plug-ins synchronously.
All of these ran at normal scheduling, nice 0. Meanwhile the JACK callback
and the live helpers (ADR-0024) run SCHED_FIFO at the server's priority
(83 under PipeWire). On a busy desktop (a build, a browser) a normal-priority
renderer gets only a fair share of each CPU. If a job's helpers are
descheduled, the job overruns, the ring drains, and an anticipation miss
becomes an audible dropout. A bridged plug-in's audio thread at normal
priority causes priority inversion behind any renderer that waits for it.

Three facts about the target workstation shaped the choice:

- **Raising nice does not help.** With the cgroup CPU controller (systemd
  desktops), nice only counts against threads in the same cgroup, and
  terminals and services get their own. A build in another terminal is
  unaffected by Quire's nice.
- **The ceiling is 50.** The kernel runs its threaded interrupt handlers
  (disk, GPU, network) and its display commit workers at SCHED_FIFO 50.
  Render bursts of several milliseconds above them would delay I/O and
  compositor frames.
- **Other applications' audio comes next.** rtkit grants their audio threads
  low real-time priorities (at most 20). A browser's audio playback must not
  glitch because Quire renders ahead.

## Decision

1. **A fixed low real-time level.**
   - The renderer and the background pool's helpers run SCHED_FIFO at
     `Engine::kBackgroundPriority` = **10**.
   - It is lowered to one below the callback's priority when that is lower,
     and to `RLIMIT_RTPRIO`.
   - Below 1 they stay normal.
   - Any FIFO level beats normal-priority desktop load equally, so the lowest
     useful level is the friendliest. It sits below the callback, the live
     helpers and the kernel's interrupt threads, and no higher than the audio
     threads rtkit grants other applications. The 200 ms render-ahead never
     needs to outrank those.
   - Equal-priority round robin brings nothing: the kernel's 100 ms timeslice
     is ten times a render job, and the CPU cap below keeps the pool smaller
     than the CPU count.
2. **Only under a real-time callback.**
   - Elevation follows the callback: a normal callback (the dummy backend, a
     JACK server without RT) leaves everything normal.
   - The engine never elevates beyond the callback's class.
   - `EngineOptions::background` forces the choice for benchmarks and tests.
3. **Process-local, checked, all or nothing.** On the main thread, after the
   backend starts, the engine sets the policy with
   `pthread_setschedparam` and verifies it. It refuses to elevate, leaves
   every background thread normal, and reports an actionable warning when:
   - the priority would fall below 1;
   - `RLIMIT_RTTIME` is finite (rtkit-managed processes: a renderer catching
     up can run longer than that without blocking and would be killed);
   - the renderer plus its helpers would leave fewer than two of the allowed
     CPUs to the rest of the system (a sustained overload must not take every
     CPU);
   - or any thread fails to take the policy.

   It does not use JACK's thread API here: on systems without an RT limit it
   goes through rtkit, which requires a finite `RLIMIT_RTTIME`. Threads end at
   `stop()`, so nothing is restored. CPU affinity is never changed.
4. **No inversion through the bridge.**
   - A bridged instance's audio thread takes the engine's background policy
     when processing starts.
   - Protocol version 10 adds `audioPolicy` and `audioPriority` to the
     shared-memory header. The host writes them before `StartProcessing`,
     from a process-wide value the engine sets while it runs (normal
     otherwise). The bridge applies them on its audio thread.
   - A bridge thread runs only while its caller waits for it, so it adds no
     concurrency beyond the pool.
5. **Reported and named.**
   - `engine.start` and `engine.status` report each class's applied policy
     (`scheduling`: callback, live, renderer, background).
   - Every audio thread the host creates carries a role name (`audio-render`,
     `audio-bg-N`, `audio-live-N`, `audio-bridge`, …).

## Consequences

- Renderer jobs no longer compete with normal-priority load for CPU time, as
  long as the session fits the pool.
- In steady state the pool is busy for one job per render block, a few
  milliseconds, then parks. Normal tasks on those CPUs wait up to one job
  while the scheduler moves them elsewhere.
- In sustained overload the pool takes its CPUs almost continuously, but at
  least two allowed CPUs stay free of it. Kernels with RT throttling enabled
  also cap it.
- Systems without real-time permission keep the previous behaviour, with a
  warning when the callback itself is real-time.
- An offline render that runs while the engine plays shares bridge
  processes' policy; its bridge threads still run only while it waits.

## Alternatives considered

- Nice levels: ineffective across cgroups.
- FIFO at the callback's priority minus 20 (63 under PipeWire): above the
  kernel's interrupt threads.
- JACK's `jack_acquire_real_time_scheduling`: on rtkit systems it implies a
  finite `RLIMIT_RTTIME`.
- Priority-inheritance futexes for the bridge: a protocol rewrite.
- A spawn-time bridge argument: bridges outlive engine restarts.
- Restricting threads to the faster cores: the pool is larger than that set,
  and the RT scheduler already places FIFO threads on the least loaded CPUs.
- A Preferences override: no demonstrated need.
