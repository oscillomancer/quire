# ADR-0052: Track edit groups and VCA control strips

**Status:** Accepted, 2026-10-28. Contracts: session-model.md, engine.md and gui.md.
Builds on ADR-0008/0009 (model/history/formats), ADR-0019/0020 (strip staging)
and ADR-0051 (control timing). The native Console 9 gain ceiling is unchanged.

A group is an ID-keyed model object, not an audio-routing relationship. It
links explicit attributes for manual edits; overlap expands the active
connected component once per attribute and invocation. Volume changes are
relative in dB, pan changes relative in percentage points, and discrete
controls copy the requested value. Selection remains UI state. Item groups
remain separate from track groups. Matching edit items have identical start
and end sample boundaries and corresponding lanes; no transient detection or
nearest-item guess is implicit.

A group may own a VCA: an audio-free mixer control with volume, mute and solo.
Its gain combines with the member's own fader/automation before Console 9
encoding, or at the master's plain fader. Stored member values never move.
The requested effective dB sum is clamped to the existing native Console 9
top (gain 4, approximately +12.0412 dB); queries expose requested and applied
levels. A silence contributor remains silence despite another's boost.
Auxiliary source channels remain outside the fader (ADR-0020).

VCAs may control other VCAs through a separate acyclic graph. A shared
ancestor contributes only once, with contributions ordered by numeric group
ID. A track has at most 32 unique controllers. Group suspension affects
linked editing, not VCA sound. Volume and mute use ordinary envelopes;
solo uses the existing track solo reachability rules and is not automatable.
No VCA audio node, latency constraint or bridge protocol is introduced.
Each compiled consumer owns its automation cursors and uses the timing of
the fader or tap where it applies the control. Existing executor ownership,
smoothing, Console adjacency and deterministic processing remain mandatory.

Project schema 2.2 makes `groups` a typed collection. Earlier versions kept
arbitrary data there. Loading them moves that JSON value without interpretation
to `extensions.legacy_track_groups` (an unused numbered suffix on collision),
warns, and starts with no active track groups. Old item grouping tokens retain
their independent meaning. The group's ID allocation does not reuse them.

The GUI adds context-menu operations, small membership tabs and VCA strips;
there is no new groups panel. Alt/Option while beginning a fader/pan gesture
adjusts that member alone. Full control remains available through commands.
