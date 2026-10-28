# ADR-0051: Latency compensation by read-ahead

**Status:** Accepted, 2026-10-28.

Amends [ADR-0014](0014-recording-and-live-nodes.md) decision 6,
[ADR-0018](0018-sidechain-inputs.md) decision 3 and
[ADR-0020](0020-auxiliary-channels.md) decision 4. Contract:
`docs/specs/engine.md` §1.3. Preserves ADR-0012/0024's executor ownership,
ADR-0019's Console 9 staging and ADR-0045/0046's device-time graph.

## Context

The forward latency solve pads every summing input to the latest arrival,
including at a live master. A playback track with 1000 samples of latency
therefore delays an otherwise zero-latency monitored track by 1000 samples.
The claim in engine.md §1.3 and ADR-0014 decision 6 that monitoring was not
padded to match playback did not describe the implementation. Recording also
never separately subtracted ordinary monitored track/bus/master latency,
despite the older prose.

Timeline audio, MIDI and automation are available in advance; device audio
and MIDI exist only now. Scheduling known sources ahead by their own
remaining path latency removes padding caused by unrelated playback. This
must not change with record arm, monitoring or executor classification.

## Decision

### Timing and the backwards solve

Keep three existing device-sample quantities: compensated chain latency
`C(n) = latChain`, forward input arrival `A(n) = latIn`, and the main output's
startup shift `L = A(main) + C(main) = outputLatency`. `A` remains a diagnostic
and supplies the output anchor; it no longer controls source timing.

Add `S(n) = timelineShift`. For paths reaching main, downstream latency is
`D(n) = L - S(n)`. At processing position `t`, known material at the node
input comes from `t - S(n) = (t - L) + D(n)`. Device input enters now without
this source shift. Input FX remain outside graph PDC and add their physical
latency. PDC-ignored stages retain their existing physical wet/dry and pin
delays while contributing zero to `C`; they can still arrive late.

After activation and chain layout, solve backwards from main. For each
connection `u -> v`, let `a` be source latency up to its tap and `b` the
latency before its destination stage. Main/aux input sums have `b = 0`;
keys use `stage.before`. Pre-FX taps have `a = 0`, other main taps use `C(u)`.
Auxiliary source channels use `C(u)` regardless of the route's main tap.
Each auxiliary link contributes its own constraint; aux-only routes add no
main-pair constraint.

```
D(main) = C(main)
D(u) = max(a + D(v) - b)
S(u) = min(S(v) + b - a)
delay(u -> v) = S(v) + b - S(u) - a
```

Anchor `S(main) = A(main)`. The old forward solution satisfies every
constraint, so solved shifts are at least `A` and delays are nonnegative.
A single path needs no edge padding; fan-out retains padding on shorter
paths. A live source feeding main directly and through a latent bus keeps
its dry and wet aligned. An independent latent source at their sum adds no
monitoring delay. Internal parallel-group compensation stays unchanged.

Use the **main master anchor for every plan**, realtime and offline. Nodes
not upstream of main receive a forward schedule no earlier than `A`, against
already solved source shifts. They must not constrain the audible main path.
This covers disconnected nodes, alternate masters and outgoing keys from
masters without assuming main is last in topological order. Offline output
selection remains after preparation; trim the selected tap at its own
`S(selected) + tapOffset`. Do not introduce a different solve for stems or
alternate masters.

### Timing consumers

| Consumer | Shift in processing coordinates |
|---|---|
| Audio items, take output spans/fades/envelopes, track MIDI | `S(n)` |
| Take source audio/MIDI | `S(n) - take.latency` |
| Track-stage parameters, transport and plugin-output timestamps | `S(n) + stage.before` |
| Take-stage parameters, transport and plugin-output timestamps | `S(n) - take.latency + stage.before` |
| Strip fader/pan/mute | `S(n) + C(n)` |
| Source-owned send-encoder controls | `S(source) + sourceTapOffset` |
| Unencoded key controls at the destination | `S(target) + stage.before` |
| Main startup, playhead and recording base offset | Existing `L` |

Give plugins their stage-input transport at unity rate too. Previously only
rate-enabled plans corrected that transport. Preserve transport flags and
the existing device steady-time coordinate.

For playrate, apply device shifts **before** the fractional-clock lookup.
Source conversion, MIDI, note matching, take spans and automation use the same
shift. Recompute source-ahead/behind and clock-window margins from `S`, take
latency and stage offsets, retaining canonical source quanta, extra clock
headroom and prepared-locate recovery. Moving a compensated stretch source
requires fresh converter history to preserve phase. Adding track latency
can now move its sibling sources too, and therefore trigger that existing
recovery, just as changing take latency can.

### Executors, startup and recording

Nothing in the solve reads live/arm/monitor classification. Both executors
process the same schedule. Preserve ownership, plan adoption, retained state
and matching delay-history transfer when monitoring changes.

Frontier rings are addressed by monotonic **stream frames**, not loop-wrapped
timeline samples. Keep producer and consumer at matching stream positions;
apply residual edge padding at the consumer. The source shifts already
express logical read-ahead relative to the retained global startup shift.
Even a path longer than the usual 200 ms render horizon needs no additional
future frontier read, so no renderer-lead fallback is required. Keep existing
ring capacity, priming, miss counting and wake rules. Auxiliary links remain
within one executor, since frontier rings carry only the main pair.

Keep Console 9 adjacency: encoded edges only sum, map channels and delay.
All solve/storage/clock preparation runs off the audio path. Callback and
worker execution allocates nothing and never blocks or calls a synchronous
bridge; the existing bridge restrictions remain.

Keep capture placement at callback play position minus `L` and the backend's
output plus input latency. Input-FX capture retains its existing initial
latency removal and tail padding. Do not separately subtract `D` or ordinary
monitored-chain latency: capture timestamps the physical performance against
playback, not the later monitored return. Keep unity-only recording admission.

### Compatibility

The requirements are exact sample alignment of timeline material,
deterministic renders, and realtime output bit-identical to offline output
of the same plan in both executors, including bridged playback. Historical
bytes from the forward-padding engine are **not** an invariant. Moving
processing relative to content can change Console 9 noise/dither phase,
free-running or other per-frame state and the 32-frame parameter grid's
phase. Correcting unity-rate plugin transport can also change a
transport-sensitive plugin's output. These changes do not authorize timing
errors or differences between executors.

## Consequences

Monitoring latency comes from its own downstream plugins and fan-out
alignment, plus input FX and device latency. Unrelated playback latency does
not add padding at a common sum. Startup/playhead compensation remains.
Arming changes execution ownership without changing the timing solution.

Graph diagnostics retain `lat_in` as forward arrival and add `timeline_shift`;
`lat_chain` retains its meaning. The implemented diagnostic is
`OfflineResult::graph`, not the previously documented, unregistered
`engine.graph` command.

## Alternatives considered

Keeping arrival-time padding retains the bug. Special-casing live nodes
changes timing when armed. Duplicating live and timeline plugin chains
changes nonlinear mixing and plugin state. Advancing only items/MIDI leaves
controls and plugin time behind. Future frontier reads would need a bounded
lead/fallback policy that the retained common processing clock avoids.

## Implementation notes

`host/engine/src/plan.cpp` prepares the reverse schedule and applies source,
control and plugin-transport shifts; `plan.h` stores `timelineShift`.
`offline.cpp` trims the selected tap and reports its schedule. Playrate uses
the existing source/clock machinery with the new shifts.

Portable `test_pdc.cpp` covers unrelated monitoring latency (fails with the
old solve in both executors), RT/offline equality and determinism, arm/monitor
continuity, live dry/bus fan-out, recording placement, non-grid automation
and playrate, plus stage-input plugin transport at unity rate. Existing
engine tests cover taps, keys, auxiliary links, parallel chains, takes,
PDC-ignore, loops, Console and bridges.
