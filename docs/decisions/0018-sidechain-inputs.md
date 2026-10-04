# ADR-0018: Sidechain inputs

**Status:** Accepted, 2026-09-28. Build prompt Phase 4, "Processing graph:
tracks, sends, sidechains, busses, and master". Builds on ADR-0012/0014
(executors, the live region), ADR-0013 (the bridge), ADR-0017 (VST3).

## Context

Compressors, gates and vocoders take a second audio input (the key). CLAP
plug-ins declare it as a non-main input port, VST3 plug-ins as an auxiliary
input bus. The engine is stereo end to end and gives a plug-in one input.
Two defects follow today: a CLAP plug-in with a sidechain port is called
with `audio_inputs_count = 1` although it declared two ports (it may read
`audio_inputs[1]`), and nothing can feed the key.

REAPER reaches the key through multichannel tracks: a send maps channels
1/2 onto the destination's 3/4 and the plug-in's pin map connects 3/4 to its
sidechain pins. That model (flexible channel counts, pin maps) is in the
spec and stays the long-term generalization, but it needs every buffer in
the engine to become N-channel. An LLM (and most users) think of a sidechain
as "send the kick to the bass compressor's key".

## Decision

1. **A route can end at a plug-in's key.** `RouteEnd` gains `fx`: a route
   `{from: {track}, to: {fx}}` delivers the source's tap (pre_fx,
   pre_fader or post_fader, with the route's volume, pan, mute and
   automation) to that instance's sidechain input. The destination track
   is the one whose chain holds the instance; take FX are not targets yet.
   Several routes into one key sum. A key route does not feed the track's
   input sum.
2. **Hosts give every port a buffer.** `host::Instance` reports
   `sidechainChannels()` (the first non-main audio input: CLAP the first
   input port without `CLAP_AUDIO_PORT_IS_MAIN`, VST3 the first auxiliary
   input bus; 0 when none) and `ProcessBlock` carries `sidechain` /
   `sidechainChannels` (null: silence). CLAP: `audio_inputs_count` and
   `audio_outputs_count` are the declared port counts; the key port gets the
   block's key or silence, other extra inputs silence, extra outputs scratch
   (preallocated at activation, `constant_mask` set for silence). VST3: the
   key bus is activated only while a route feeds it (plug-ins may treat an
   active key bus as "use the external key"); the engine asks for it with
   `setSidechainActive(bool)` before activation, and a change replaces the
   instance at the next plan (as a class change does). The bridge carries
   the key channels in its data plane (protocol 4).
3. **Graph and delay compensation.** A key route is an edge into the
   destination node that names a chain stage. Ordering is unchanged (the
   destination runs after its sources); cycles are rejected as for sends.
   The key is aligned with the stage's input, whose latency is the node's
   input latency plus the latencies of the compensated stages before it:
   `latIn = max(main edges' source latency, key edges' source latency −
   before(stage))`, a main edge is delayed by `latIn − source`, a key edge
   by `latIn + before(stage) − source`. The stage sums its key edges into a
   preallocated stereo buffer before it processes.
4. **Mute and solo.** A key edge follows its source's mute at a post-fader
   tap (as a send does) but ignores solo: soloing the bass must not stop its
   compressor from ducking under the kick. A pre-fx or pre-fader key also
   ignores the source's mute.
5. **Executors.** A node fed by a live node is live (keys included). An
   anticipative source keying a live destination is a frontier edge (the
   ring carries the tap as for sends). Offline, real-time and anticipative
   renders stay identical.
6. **Commands.** `route.add {from, to?, fx, tap, volume_db, …}` with `fx`
   (`to` may be omitted; if given it must hold the instance); `route.list`
   and `route.set` show and change `fx`. Errors: `route.no_sidechain` (the
   plug-in has no key input), `route.cycle`. `fx.describe` reports
   `sidechain_channels` when the plug-in has a key.

## Consequences

- Sidechaining works for CLAP and VST3, in-process and isolated, with PDC.
- CLAP plug-ins with extra ports get well-formed process calls.
- Multichannel tracks and pin maps (spec §1.3) remain open and will
  generalize this: a key route is then a send into the channels a pin map
  connects to the key. The project format keeps `to.fx` meaningful then.

## Alternatives considered

Multichannel tracks and pin maps first (the REAPER model): the complete
answer, but a rewrite of every buffer path for the one case users ask for
most, and a harder model for an LLM to use. Always activating the VST3 key
bus: simpler, but some plug-ins then key from silence and stop working as
plain compressors.

## Implementation notes (2026-09-29)

- **Unsoloed post-fader keys.** Audible post-fader output retains its solo
  gate. Sources used by post-fader keys also compute an unsoloed tap through a
  separate, identically seeded Console 9 encoder and fader (including mute).
  Only key edges read it; no signal crosses Console 9 twice. The source owns
  that tap, including publication to frontier rings, so a live destination
  sees the same key samples as an offline destination.
- **Key restarts.** `Engine::load` compares each VST3 instance's key
  activation with the new plan's; a change stops the instance on the audio
  thread and a fresh one is activated with the new flag. CLAP instances
  keep running (the key port always gets a buffer).
- **Delay compensation** is tested sample-exactly for four latency layouts
  (latency before and after the keyed stage, on the key source, on both),
  in the offline renderer and in both real-time executors (live monitoring
  included).
- **Route edits.** Removing a track or an FX instance removes the key
  routes that end there; a key route whose plug-in no longer has a key
  input (e.g. after a class change) stays in the project and the plan
  ignores it with a warning.

