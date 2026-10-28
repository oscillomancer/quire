# ADR-0051: Latency compensation by read-ahead

**Status:** Proposed, 2026-10-28. The maintainer has agreed the read-ahead
direction; the implementation plan and compatibility questions below await
review. This record does not claim an implemented change.

Amends [ADR-0014](0014-recording-and-live-nodes.md) decision 6,
[ADR-0018](0018-sidechain-inputs.md) decision 3 and
[ADR-0020](0020-auxiliary-channels.md) decision 4. Contracts to update with
implementation: `docs/specs/engine.md` §1.3, timing/automation, executor,
recording and playrate sections. Preserves ADR-0012/0024's executor ownership,
ADR-0019's Console 9 staging and ADR-0045/0046's device-time graph.

## Context

At the starting revision, `e45fc9b6`, `prepare()` in
`host/engine/src/plan.cpp` solves input arrival times forwards. A node's
`latIn` is the latest incoming tap, corrected for the destination stage;
every faster incoming edge receives a delay. The main master's input is
treated in exactly the same way, even when it is live. A playback track
with 1000 samples of compensated latency therefore delays an otherwise
zero-latency monitored track by 1000 samples at their common master.

The claim in engine.md §1.3 and ADR-0014 decision 6 that monitoring is not
delayed to match playback describes the intended behavior, not the current
implementation. The forward solve also appears explicitly in ADR-0018/0020.
History of the solve (`c11a82bb`) confirms that input FX, parallel stages and
PDC-ignore were layered onto this same arrival-time rule.

Timeline audio, MIDI and automation are available in advance. Device audio
and MIDI are available only now. Compensation should schedule each known
source early enough for its own downstream path, retaining padding where
one signal must traverse paths of different latency. The timing solution
must not depend on record arm, monitoring or executor classification.

## Decision proposed

### 1. Keep separate timing quantities

Use device samples throughout the latency solve. Preserve:

- `C(n)`: the existing compensated chain latency, `latChain`, including
  internal parallel alignment and oversampling. `fx.pdc = ignore` continues
  to exclude a stage from this quantity without removing its physical
  wet/dry or pin delay.
- `A(n)`: the existing forward input arrival, `latIn`. Retain the forward
  calculation for output anchors and compatibility diagnostics; it no
  longer determines the implemented edge padding or source timing.
- `L = A(main) + C(main)`: the existing `Plan::outputLatency`. Keep its
  startup, audible-playhead and main-render trimming meaning.

Add an explicitly named `timelineShift`, `S(n)`, and a downstream quantity
`D(n) = L - S(n)` for paths to the main output. At processing position `t`,
the audible main-output coordinate is `t - L`; node input timeline material
is evaluated at `t - S(n) = (t - L) + D(n)`. A positive `S` is thus a shift
in the existing processing coordinates, not a delay applied to device input.
Take sources additionally run ahead by their own chain latency.

Input FX remain outside the graph solve. Their actual latency adds to the
monitoring path and their recorded signal retains its existing correction.
An ignored plug-in can likewise add uncompensated physical latency; `D` is
the compensated path latency, not a measurement of such an instance.

### 2. Solve backwards over all audio dependencies

Compute the new values in `prepare()`, after activation and `layoutChain()`
have established every stage's `before` and compensated latency, and before
shifting envelopes, compiling item/event positions or sizing clock windows.
Walk `p.nodes` in reverse topological order and inspect destination `in`
edges; `Node::out` lists only source-owned send encoders and is not a complete
outgoing adjacency list.

For a connection from node `u` to node `v`, let:

- `a` be latency within `u` up to the source tap: zero for pre-FX,
  `C(u)` for pre/post-fader. An auxiliary source channel uses `C(u)` even
  if the route's main-pair tap is pre-FX.
- `b` be latency within `v` before the destination: zero at the input sum
  or an auxiliary link, `stage.before` for a plug-in key.

For the main-output dependency graph, the reverse recurrence is
`D(u) = max(a + D(v) - b)` over its outgoing connections, with the main
output anchor `D(main) = C(main)`. Include each auxiliary link separately;
an aux-only route supplies no main-pair constraint. Pre-FX connections must
not accidentally include the unused source inserts.

The equivalent implementation uses latest permitted input shifts:

```
S(u) <= S(v) + b - a
delay(u -> v) = S(v) + b - S(u) - a
```

Set each node to the minimum of its outgoing upper bounds. Every resulting
edge/link delay is nonnegative. For a single path to the output it is zero;
for fan-out it pads the shorter path. A live track feeding a direct master
path and a 200-sample bus therefore retains 200 samples on its direct path.
An independent latent track joining the same master adds no such delay.
Summation order, maps and the internal chain program stay unchanged.

Plans can contain several masters, masters with outgoing key routes, and
disconnected nodes. Do not assume the last topological node is the output.
Make the observed output explicit before preparation: the callback currently
emits only `mainMaster`, while an offline render selects one master or stem
tap. Solve backwards through that output's predecessors, including key and
auxiliary dependencies, with its old tap arrival as the output anchor.
For the other nodes, complete a forward schedule no earlier than their old
`A`, respecting the already solved source shifts; they cannot feed the
selected output or they would be in its predecessor set. This keeps all
edge delays nonnegative without letting an unused output constrain the
audible path. `D` is meaningful relative to the selected output, not a
fictitious path to it for a disconnected strip.

Do not anchor every master at a different old arrival in the same reverse
solve: a live source shared with a low-latency alternate master would then
retain unrelated-track padding on its main-master branch. Simultaneously
audible independent hardware masters would require an explicit common-output
clock policy; the current engine emits one stereo master.

For offline masters/stems, move output selection ahead of `prepare()` and
retain the existing upstream-session selection for isolated taps. Use the
selected tap's old arrival as that render's startup anchor and trim at
`S(selected) + tapOffset`. Each chosen output keeps its existing startup
amount; it need not share the realtime main master's internal schedule.
Cover alternate masters, masters used as key sources and pre-FX stems,
including their playrate clock origins. The forward solution satisfies the
reverse constraints, so the resulting `S` is no earlier than `A`.

No part of this solve reads `live`, `anyLive`, `heldLive`, arm or monitor.
No source conversion or gain is inserted between a Console 9 encode and
decode; encoded edges still perform only sum, channel mapping and padding.

### 3. Map every existing timing use deliberately

Keep `latIn` as `A` rather than silently changing its meaning. Runtime
timeline consumers move to `S` as follows:

| Existing use | New timing |
|---|---|
| Clip `start`, `deviceShift`; take output span, gain/fades/envelopes; track timeline MIDI | `S(n)` |
| Take audio/MIDI input and take-stage parameter lanes | `S(n) - take.latency`, plus `stage.before` at a stage |
| Strip fader/pan/mute and post-chain taps | `S(n) + C(n)` |
| Source-owned send-encoder automation, including followed strip controls | `S(source) + sourceTapOffset` |
| Unencoded key-edge automation at its destination | `S(target) + stage.before` |
| Track stage input, parameter lanes, plugin-output timeline timestamps | `S(n) + stage.before` |
| Edge and auxiliary delay storage | The backwards-solved delay in §2 |
| Main audible playhead, startup shift, recording base offset | Existing `L` |
| Offline selected-tap trimming | Selected input shift plus tap offset |

The intended plug-in transport is the timeline at its stage input, including
take-stage lookahead. Preserve playing/recording/loop flags and a monotonic
device-domain steady clock. Current unity-rate transport differs from this
rule; the compatibility gate below must be resolved before changing it.

For playrate, substitute `S` for input-arrival shifts in `mappedAt`,
`processTake`, `rateEvents`, clip device shifts, note release matching and
source adoption. Subtract the shift **before** the fractional-clock lookup;
do not add a device-frame latency to the resulting timeline samples.
Preserve canonical compensated-source quanta and prepared control history.

Recompute `rateSourceAhead` from the maximum positive `take.latency - S`,
and `rateSourceBehind` from `S` and `S - take.latency`. Size
`rateDeviceMargin` for both sides of all source/stage queries, including
`L + rateBackendLatency`, `S + C`, and take/stage offsets. Retain the existing
extra clock headroom and prepared-seek recovery. Allocation and source
priming remain on the control thread, before publication.

### 4. Preserve the renderer's stream coordinate and bounded lead

`ChunkRing::pullAt` actually addresses monotonically increasing **stream
frames**, not loop-wrapped timeline samples. The renderer publishes taps at
`streamLocal_`; the callback requests `liveStream_`. Both executors run the
same shift/delay solution, so keep these equal-coordinate frontier reads.
The edge's residual delay is still applied by its consumer. Auxiliary links
remain within one executor, as required by ADR-0020; their data does not fit
the stereo frontier rings.

In this representation a large logical `D` does not demand a future ring
sample: the timeline read-ahead is already expressed relative to the
retained global startup shift `L`. A graph latency greater than the normal
200 ms render horizon does not, by itself, exhaust renderer lead. Preserve
the configured/effective `aheadFrames_`, ring capacity, priming and wake
rules. Test latency beyond that horizon explicitly instead of imposing an
unnecessary 200 ms cap on compensation.

If implementation requires an additional frontier advance `r`, its usable
lead `H` must account for whole render blocks and ring capacity. Limit that
advance to `H` and compensate the remaining `max(0, r - H)` by padding the
joining live side, preserving alignment at the cost of monitoring latency.
Such a fallback must be part of the compiled topology/horizon solution for
every potentially live boundary, including in the callback/offline reference;
it cannot be enabled by arming a track. Never invent a runtime delay on an
underrun or wait in the callback. With the chosen common stream coordinate,
`r = 0`; no new horizon fallback is expected to be needed. This distinction
from a raw `D > render_ahead` test needs review before implementation.

### 5. Preserve recording placement and plan ownership

`Engine::startRecording()` currently supplies the recorder with
`L + backend.outputLatency + backend.inputLatency`. `Recorder::capture()`
places its first frame at capture position minus that amount. Processed
input-FX capture drops its own initial chain-latency frames and pads the
file's tail. Ordinary monitored track/bus/master chain latency is **not**
subtracted separately in current code, contrary to the older prose.

Retain that placement rule: it timestamps a physical performance against
the playback the performer heard. Since `L` and output origin remain fixed,
the read-ahead solve does not by itself justify adding or subtracting `D`.
Record and reopen a take containing a known input impulse against a known
playback cue, with nonzero plan/device latencies and with/without input FX,
in both executors. Also cover MIDI and loop-run placement using existing
fixtures. A different policy that records the later monitored return would
be a separate behavior change, not an incidental latency fix.

Preserve the existing two-executor adoption, ownership and retirement
protocols. Delay histories carry when the solved lengths and route identity
match; source, note, Console and converter state must survive a pure
arm/monitor edit. Equal timing fields are necessary but are not sufficient
proof of continuous playback during an instance ownership handoff.

## Compatibility questions and implementation gates

The reverse equations prove signal alignment, not that the current engine's
bytes are invariant under moving an edge delay into source timing. Static
inspection exposes three concrete issues:

1. **State and seeded noise.** Console stages run every processed sample and
   reset at a common locate (`processNode`, `locateClass`, `strip.h`). With
   1000 samples of padding, a low-latency channel's encoder output from
   processing frame zero reaches the master at frame 1000. Merely removing
   that padding and shifting its item makes frame 1000's encoder state
   reach the master instead. Seeded noise/dither, free-running instruments,
   silence-generated output and stateful startup are not generally invariant
   under that substitution. Preserving old bytes needs an explicit DSP
   phase/preroll treatment, not just different item indices or RNG seeds.
2. **Automation sampling.** `automateStage()` samples the 32-frame parameter
   grid from processing position and project origin, in addition to exact
   envelope points. Moving a lane by a non-multiple of 32 can change which
   parameter values meet a given audio sample unless its sampling phase is
   carried too. Exercise non-grid latency, not only the 64-sample fixture.
3. **Transport semantics.** `runStage()` compensates transport by
   `Stage::inputLatency` only when `rateEnabled`; the unity path passes the
   shared transport unchanged. Supplying stage-input transport at unity is
   the requested timing rule, but is observably different for a plug-in that
   uses transport. Arbitrary transport-sensitive old output cannot simply
   be declared byte-identical after this correction.

Before changing the solve, capture baseline render bytes and pin these
cases. Keep all existing expected results and equivalence tests unchanged.
Determine whether preserving historical DSP phase and transport is compatible
with the requested timing contract; report any necessary contract choice to
the maintainer rather than silently redefining "unchanged output". Existing
RT/offline tests often generate their offline reference from the same current
implementation, so they alone cannot establish equality with the old engine.
Any changed golden or existing expected output stops implementation for review.

Two other corrections belong in the implementation documentation:

- The renderer horizon bounds stream availability, not arbitrary access to
  timeline media (§4); a raw read-ahead cap would impose needless live delay.
- `engine.graph` is named in the spec and `OfflineResult` comment, but no
  such command is registered at this revision. The implemented graph text is
  `OfflineResult::graph` in `offline.cpp`. Keep `lat_in` explicitly historical
  if retained; document new shift/downstream fields with the public header.
  Do not add an unrelated command to fulfill an outdated spec description.

## Validation and consequences

After review, first add the smallest plan/impulse regressions proving the
bug, then integrate the reverse solve and timing consumers. Required checks:

- An unrelated latent playback track cannot increase a monitored impulse's
  latency, with anticipation on and off and with live helpers. This is
  expected to fail on the starting revision; it has not yet been run.
- Silent monitored input plus latent playback equals the offline reference
  exactly in both executors, including bridged playback and Console 9 on/off.
- An arm/monitor toggle leaves `S`, `D` and every edge/link delay unchanged
  and leaves continuous playback samples unchanged through adoption.
- Recording placement and input-FX alignment follow §5; dry/wet fan-out
  stays aligned, with pre-FX, keyed-stage and auxiliary variants.
- Main/alternate masters, stems, take chains, PDC-ignore, non-grid latency,
  seeks/loops, scalar/enveloped playrate and latency exceeding 200 ms retain
  their contracts. No test decides success by wall-clock duration.

Run dev plus the engine checks under ASan/UBSan, TSan and RTSan; build the
whole GCC preset. Use a short `TMPDIR`, run `tools/ci/format.sh`, and update
engine.md, relevant public comments, generated command reference if a command
actually changes, and `STATUS.md` with implementation. The current manual
has no PDC/monitoring explanation; add concise user-facing guidance when the
behavior lands. No new dependency or platform-specific mechanism is needed.
Linux results do not establish macOS/Windows behavior: native CI and review
by someone other than the author remain integration gates before merging.

Step 1 changes only decision records and their index. No implementation,
engine test result, native validation or completed phase-compatibility proof
is claimed here. Do not push or dispatch CI as part of this work package.

## Alternatives considered

Keeping arrival-time padding preserves the bug. Special-casing only live
nodes makes timing jump on arming and changes the playback graph with its
executor classification. Duplicating timeline and live plug-in chains changes
nonlinear mixing and plug-in state semantics. Advancing only MIDI/items while
leaving envelopes, take processing or plug-in time behind misaligns controls.
Unbounded future frontier reads break the fixed render horizon. Increasing
that horizon whenever a plug-in is latent adds control latency unnecessarily
when the retained common processing clock already supplies the needed data.
