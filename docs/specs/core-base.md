# core-base: main-thread wake relay

**Decision:** ADR-0029. Public header:
`supple/base/main_thread_wake.h`, namespace `supple`.

`MainThreadWake` turns real-time-safe notifications into a pollable Linux
eventfd. It owns one parked relay thread. It is noncopyable; construction
and destruction run off audio. Every producer must stop or join before the
relay is destroyed. Destruction wakes and joins the relay, then closes its
descriptor.

- `fd()` returns the owned, nonblocking, close-on-exec eventfd, or -1 if
  eventfd creation failed. The caller checks this before subscribing
  producers or waiting. Callers borrow it for polling; they neither close it
  nor read it directly in normal service.
- `notify()` is `SUPPLE_RT`, callable by concurrent producers. Publish work
  before calling it. Notifications coalesce until main drains: the first
  producer sets a pending bit, advances a private futex sequence and calls
  the sanctioned `rt::wake`. Later producers still publish through the
  pending bit, but do not wake the relay again. Only the non-RT relay writes
  the eventfd; producers never write descriptors, allocate, lock or wait.
- `drain()` is non-RT, called by one main thread **before** consuming the
  associated queues or flags. It returns false when no event was read. On
  success it acquires coalesced publications, clears the pending bit and
  returns true. A request arriving during subsequent service signals
  another pass. Interrupted reads are retried. Do not clear/re-arm by
  separately reading the descriptor.

The notification stores no application work. Queues and flags retain their
own capacity, ordering and lifetime contracts. A wake does not mean the
producer's work is complete or authorize reclaiming audio-owned objects.
There is no periodic relay timeout.

The bridge uses this shared relay through optional
`host::Instance::setMainThreadWake(MainThreadWake&)` subscriptions (sandbox
spec §1). The app's in-process instances are not subscribed yet. EngineApp
service scheduling and bridge-to-parent notifications are separate work.
