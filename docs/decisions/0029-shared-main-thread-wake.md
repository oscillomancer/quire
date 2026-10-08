# ADR-0029: Shared main-thread notification relay

**Status:** Accepted, 2026-10-08. Spec: `docs/specs/core-base.md`.
Builds on ADR-0024's private futex wake exception and ADR-0013's bridge.

## Context

The bridge has an audio-safe, coalesced futex-to-eventfd relay. The engine
app will need the same primitive. Keeping it inside clap-host would make
lower-level consumers depend on host code, or lead to duplicate relays.

## Decision

Move the existing relay to core-base as `supple::MainThreadWake`, with public
header `supple/base/main_thread_wake.h` and generic thread name `main-wake`.
Move its coalescing/publication/re-arm tests with it. Keep plug-in timer
timeout helpers and their tests inside clap-host.

Preserve the existing behavior. Producers publish work and notify through
atomics and the sanctioned private futex wake. A parked non-RT thread
writes the eventfd. Main drains and re-arms before consuming work. The relay
outlives all producers, and destruction joins it before closing the fd.

The bridge continues using the shared implementation. Its wire protocol,
optional instance subscription and service loop do not change. This step
does not subscribe EngineApp or remove its timers; those changes require
their own contracts and validation.

## Consequences

Engine, host and lower-level code can share one primitive without reversing
dependencies. No new dependency or audio-thread syscall exception is needed.
The moved tests retain explicit synchronization and notification counts,
without elapsed-time pass/fail assertions.

## Alternatives considered

Leaving the relay host-private forces an upward dependency. A second relay
duplicates its concurrency and lifetime rules. Promoting the plug-in timer
helpers is unnecessary for sharing the notification primitive.
