# ADR-0025: Engine floating-point environment and first touch

**Status:** Accepted, 2026-10-08. Implementation follows in separate commits.

**Platform amendment, 2026-10-14:** [ADR-0035](0035-headless-platform-layer.md)
adds macOS/Windows mechanisms while retaining these Linux semantics.

## Context

Subnormal floats (below about 1.18e-38, −758 dBFS; doubles below about
2.2e-308) cost far more CPU than normal values on x86-64 and appear in decaying
tails and tiny signals. The stock plug-ins and the analysis code already
scope `dsp::DenormalGuard` (FTZ and DAZ) around their own DSP, but the engine
set no policy: engine arithmetic and third-party plug-ins ran in whatever
environment the executing thread had (FTZ/DAZ clear on the measured device
callback). With live helpers and background workers, which thread runs a
node depends on scheduling, so any environment a plug-in leaks into its
thread could make output depend on the thread count.

Owned audio threads also first-touch their stacks inside timed processing.

## Decision

1. **Per node, every executor.** `detail::processNode` scopes one
   `dsp::DenormalGuard` around each node: input sums, delays, Console 9
   stages, faders, oversampling, meters and every in-process plug-in
   `process()` of that node. The device callback (serial or fork-join), live
   helpers, the anticipative renderer, background workers and offline renders
   therefore compute every node with the thread's environment plus FTZ and DAZ.
2. **Restore everywhere.** The guard restores the thread's previous MXCSR
   when the node ends, on threads Quire owns and on threads it does not (the
   JACK/PipeWire callback thread). A plug-in that changes MXCSR in `process()`
   affects at most the rest of its own node, which runs in fixed order on one
   thread.
3. **The same rule outside the graph.** The bridge scopes the guard around
   each `process()` of a bridged plug-in, the offline stem decoder around its
   Console 9 decode, and the plug-in sandbox around each render block.
4. **Not guarded.** Plug-in lifecycle and main-thread calls (`activate`,
   `start/stop_processing`, `reset`, parameter flush outside `process()`),
   transport computation, copies, the callback's metronome and offline
   post-processing (normalize, limiter, dither, encoding) keep the thread's
   environment, identically in every executor.
5. **Numeric contract.** Real-time and offline output stay bit-identical:
   every executor runs each node under the same environment. Values that
   underflow inside a node now become zero instead of subnormal; normal-range
   results are unchanged. `libs/console9` itself is unchanged and still
   preserves tiny inputs in its own tests; inside the engine it runs under
   this policy. The guard exists on SSE2 targets only; other architectures
   keep their default environment.
6. **Third-party plug-ins** (in-process and bridged, CLAP and VST3) process
   with FTZ and DAZ set, in real time and offline alike. A plug-in that relies
   on gradual underflow in `process()` sees zeros below the normal range.
7. **First touch.** Audio sample buffers stay zero-filled on the main thread
   when plans and rings are built, which commits their pages. Owned audio
   threads (live helpers, background workers, the renderer, the bridge's
   audio threads) call `supple::rt::prefaultStack()` once before their first
   job; it touches 128 KiB below the caller and skips threads with less than
   512 KiB of stack left. Reserved event and voice storage stays lazily
   committed, and no plug-in code is executed early to warm it.

## Consequences

Exact output changes only where a value underflows inside a node; there is no
change to summation order, Console 9 adjacency, ownership or scheduling. The
guard costs a few MXCSR instructions per node. Pre-touched stacks cost about
128 KiB of resident memory per owned audio thread. The benefit for typical
sessions is unproven: stock plug-ins already flush, and steady-state page
faults were already zero in the baseline measurements.

## Alternatives considered

A guard per thread or per cycle needs a site in every executor and leaves a
leaked MXCSR change on the thread for later nodes, whose placement depends on
scheduling. Guarding inside the hosting library's `process()` would impose a
policy on every caller of the library; the executors own the environment
instead. Committing reserved event and voice capacity costs resident memory
in every note-target stage for an unmeasured gain.
