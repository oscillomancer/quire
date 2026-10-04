# ADR-0020: Auxiliary channels and plug-in pins

**Status:** Accepted, 2026-10-04. GUI workflow-gap log #6 (track channel
count, routes to 3/4) and #31 (FX pin maps, detector input on 3/4). Builds on
ADR-0018 (key routes), ADR-0019 (typed strips with Console 9 summing) and
ADR-0012/0014 (executors, the live region).

## Context

REAPER users reach a compressor's detector by giving a bus four channels,
sending the kick to 3/4 and pinning the plug-in's sidechain to 3/4. Quire's
strips are stereo end to end: the fader, the pan, the Console 9 encode and
decode, the meters, the parent send, the frontier rings and the bridge's data
plane all carry one pair. ADR-0018 gave plug-ins a key through key routes,
which covers "send the kick to the bass compressor's key" without channel
counts, but not the REAPER workflow, nor a plug-in that should process a
signal other than the strip's main pair.

Making every buffer N-channel would touch every executor, the Console 9
stages and the bridge, and would make the main pair's mixing semantics
(per-channel encode, pan law) ambiguous beyond two channels.

## Decision

1. **A strip is its main pair plus auxiliary channels.** `track.channels`
   is 1-8 (busses and masters at least 2). Channels 1/2 are the main pair:
   fader, pan, Console 9, meters and the parent send act on them, unchanged.
   Channels 3..N are auxiliary: linear buffers inside the strip, never faded,
   panned, encoded or sent to the parent.
2. **Routes address any channel.** A route's channel map (`channels {from,
   to}`, 1-based) may name the source's and the destination's auxiliary
   channels. Pairs between main channels keep today's matrix (on the encoded
   send). A pair that touches an auxiliary channel is a linear link: the
   route's volume, mute and polarity apply (its envelopes and pan act on the
   main pair); a source's auxiliary channel is read after its chain, a main
   channel at the route's tap (post fader: before the encode, at the strip's
   level and mute). Links into a buss's main pair are added after its decode.
   A route that maps only auxiliary pairs adds nothing to the main sum.
3. **Plug-ins have pins.** `FxInstance.pins {in, out, key}` names the track
   channels a track-chain plug-in reads (its main input pair), writes (its
   main output pair) and keys from (its sidechain input: "a detector on
   3/4"). Empty is 1/2 in and out and the key from key routes only; key pins
   add to key routes. A pinned stage's dry signal is its input pins'; the
   channels it does not write, and every auxiliary channel at any stage of a
   strip that has them, are delayed by the stage's latency so the strip stays
   aligned. A bypassed or idle pinned stage passes every channel through.
   Take FX and input FX stay stereo. Plug-ins keep their stereo main ports
   and one key port (ADR-0018): pins choose which strip channels meet them,
   so no multi-port hosting or bridge change is needed.
4. **Latency compensation covers the links**: a node's input latency is the
   maximum over its main edges and its auxiliary links, and each link has its
   own delay line.
5. **Auxiliary links stay within one executor.** The frontier rings carry
   the main pair. A strip that feeds a live strip's auxiliary channels (or is
   fed through one) is pulled into the live region with everything it feeds,
   so real-time output stays identical to the offline render.
6. **Out of range is a warning, not an error, in the plan**: a pair or pin
   beyond a strip's channels (after `track.set channels` shrinks it) is
   reported and skipped. Commands refuse such values when they are given
   (`route.channels`, `fx.pins`).

## Consequences

- The REAPER workflow works: `track.set {channels: 4}` on the bus,
  `route.add {channels: {from: [1, 2], to: [3, 4]}}`, `fx.set {pins: {key:
  [3, 4]}}`. Key routes remain the simpler form.
- Nothing changes for stereo sessions: plans without auxiliary channels or
  pins run the same code paths, bit for bit.
- The GUI offers it where it applies: Track > Channels…, the routing
  matrix's channel menu lists the pairs both ends have, and the FX chain
  shows a pins button on strips with auxiliary channels.
- Not covered: auxiliary channels on hardware outputs and renders (stems
  render the main pair), metering of auxiliary channels, more than two
  plug-in input or output channels, and pins on take FX.

## Tests

`test_offline`: a route to 3/4 keying a plug-in through key pins equals a
key route's output (up to the route's pan law) sample for sample, with
latency at the source, before the keyed stage, or both; auxiliary channels
are not heard downstream unless a pin moves them; out-of-range pairs and pins
warn. `test_realtime`: real time equals offline, rendered ahead or with the
destination live (the fix for the frontier fails without it). `test_commands`:
validation and the whole workflow over the bus. GUI: the track menu, the
routing matrix's aux pair, the FX chain's pins menu.
