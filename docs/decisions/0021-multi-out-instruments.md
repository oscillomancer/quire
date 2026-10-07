# ADR-0021: Multi-output plug-ins

**Status:** Accepted, 2026-10-06. Nate's request: a multi-sampler that plays
each sample on its own output should work, and the routing matrix should show
every channel of such a track. Builds on ADR-0018 (every declared port gets a
buffer), ADR-0020 (auxiliary channels, routes to them, pins) and ADR-0013
(the bridge).

## Context

ADR-0020 gave strips up to 8 channels and plug-ins pins, but kept plug-ins
stereo: a CLAP plug-in's output ports past the main one, and a VST3's
auxiliary output buses, got scratch buffers and were dropped. A drum sampler
with "Kick", "Snare" and "Hats" outputs (16 stereo pairs is common) could only
be heard through its main pair.

## Decision

1. **The host hands a plug-in's auxiliary outputs to the engine.**
   `Instance::auxOutputPorts()` lists the output ports past the main one
   (name, channels) in declaration order; `ProcessBlock::auxOutputs` carries
   their channels in that order (null or short: scratch, as before). CLAP:
   every non-main output port. When the plug-in offers port configurations,
   the host picks, among those with a stereo main output (and a stereo or no
   main input), the one with the most output ports. VST3: every auxiliary
   output bus, now activated. The bridge carries up to 64 auxiliary output
   channels in shared memory and their ports in Create (protocol 8).
2. **Strips grow to 64 channels** (`model::kMaxTrackChannels`): the main pair
   and 62 auxiliary channels, which keep ADR-0020's meaning (linear, never
   faded, panned or sent to the parent).
3. **`pins.out` goes on past the main pair**: an entry per auxiliary output
   channel, in order, naming the track channel it writes (0: unconnected).
   The auxiliary outputs replace their channels after the stage runs; a
   bypassed or idle stage leaves them as ADR-0020 does (delayed). Take FX and
   input FX stay stereo.
4. **`fx.add` wires an instrument** in a track's chain: its auxiliary
   outputs land on new channels after the track's own (a stereo track with
   the 3-output sampler becomes 6 channels, `out [1..6]`), as far as 64
   channels go; one undo step. Effects with extra outputs stay unconnected
   until pinned with `fx.set`, so adding one never changes a strip.
5. **The routing matrix shows every pair.** `route.matrix` reports each
   source's `channels` and `channel_names` (the ports' names on the channels
   they write). The GUI lists a row per auxiliary pair under its strip
   ("3/4 Kick"); a click there sends that pair to the destination's main pair
   (`route.add {channels: {from: [3, 4], to: [1, 2]}}`). An auxiliary pair
   does not key plug-ins (key routes read the main pair; pins read others).
   A send drag between strips still adds at most one main-pair send; sends
   from auxiliary pairs do not count.

## Consequences

- The REAPER workflow: insert the sampler, open the matrix, click "3/4 Kick"
  under the drum bus. With `route.matrix`/`route.add`, an agent does the same.
- Nothing changes for plug-ins without auxiliary outputs, nor for effects.
  Auxiliary outputs cost a copy per channel per block; unconnected ones none.
- VST3 auxiliary output buses are active even when nothing reads them.
- Not covered: main output ports wider than stereo (the host still prefers a
  stereo main), auxiliary outputs beyond 64 channels through the bridge,
  per-output meters, and a dialog to choose which outputs to connect.

## Tests

`test_clap_host`: the multi-out fixture's outputs reach the block in process
and bridged, a short list sends the rest to scratch; the VST3 fixture's
auxiliary bus is active and reaches the block (both). `test_offline`: each
output arrives on its pinned channels and routes on (levels compare exactly),
unpinned or 0 is silent, bypass silences them, out-of-range pins warn, in
process and bridged. `test_realtime`: real time equals offline, rendered ahead
or live. `test_commands`: `fx.add` grows the track after its own channels
(one undo step), effects and the input chain stay stereo, `fx.set` checks the
list. GUI: the matrix's rows, a send from a pair, main and pair sends coexist.
