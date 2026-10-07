# ADR-0022: Strip polarity and mono

**Status:** Accepted, 2026-10-06. Nate's request: every track gets a polarity
flip and a mono button ("a mono toggle is very useful for mixing, especially
on the master buss"), and no processing may sit between a Console 9 encode
and its decode. Amends ADR-0019 §5, which had moved polarity off the strip
("a utility insert does it").

## Decision

1. **Two strip fields:** `phase_invert` and `mono` (bool, default false) on
   every strip (channels, busses, masters), set with `track.set` like mute.
   Not automatable (envelopes keep `volume`, `pan` and `mute`).
2. **Where they act:** on the main pair after the insert chain and before
   the fader, so a channel's or buss's stages are `inserts → mono → polarity
   → [pre_fader] → C9 Channel`, and a master's `inserts → mono → polarity →
   [pre_fader] → plain gain/balance`. They never sit between an encode (the
   C9 Channel at a strip's end, or a send's) and the decode (the C9 Buss at
   the parent's input). Mono comes before the pan, so a mono strip is still
   placed by its pan; on the master it comes before the balance. Pre-fader
   and post-fader taps (and auxiliary links from the main pair) hear both;
   pre-FX taps and auxiliary channels 3 and up do not.
3. **Mono** is (L + R) / 2 on both sides. **Polarity** negates both sides.
4. **No clicks:** a change ramps over the plan swap's smoothing time (10 ms),
   as mute does: polarity passes through silence, mono crossfades. Locating
   resets the ramps with the fader's.
5. **1.x projects:** a strip's `phase_invert` now carries over instead of
   being dropped with a warning (`width` and `trim` still drop).
6. **GUI:** "Ø" and "mono" pills in their own colours. The TCP puts them at
   the start of the level row (before the fader and pan, so they show when
   the row does); in the mixer they share a row above the pan with
   monitoring (channels), the arm moving up under the plug-ins.
   They act on the selection like mute (`gui::SelectionEdit`).

## Consequences

- A mono check of the whole mix is one click on the master. A route's own
  `phase_invert`/`mono` (ADR-0019 §3) stays: it changes one send, the strip's
  changes the strip.
- Each switch costs nothing when off and settled; while on or ramping, a
  multiply-add per sample on the main pair.

## Tests

`test_strips`: polarity negates the output with Console 9 on and off; a mono
channel equals a channel fed the mean (its pan still places it); a mono
master's outputs match; a switch at a plan swap ramps with no step.
`test_model`: the 1.x migration carries `phase_invert`. GUI: the TCP's and the
mixer's buttons set the fields, spread over a selection, and show the state.
