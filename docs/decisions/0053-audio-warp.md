# ADR-0053: Audio warp, transient editing and prepared sources

**Status:** Accepted, 2026-10-28.

## Decision

Persist original-source transient positions and piecewise-linear take stretch
anchors in project schema 2.2. All coordinates, validation and edit transforms use
one model mapping. Keep the implemented marker object encoding; the older spec's
array description was inaccurate. Optional fields keep 2.1 and older projects loading.

Pitch-preserving warps are prepared as immutable audio before take FX. Multitrack
warps use reciprocal typed `warp_peers` membership and one multichannel Signalsmith
instance: identical anchors in independent instances do not guarantee shared phase
decisions. Groups are independent of this DSP membership. Plan timing, Console 9
staging, master playrate and ADR-0051's read-ahead solve remain unchanged.

Prepare source data on a bounded background service and adopt a complete immutable
set before plan compilation/publication. Plugin lifecycle stays on the model thread.
Retain the prior ready plan while preparing an edit, expose pending/error state,
and discard stale results. Offline workers prepare the same samples deterministically.
No file access, allocation or stretching is introduced on the real-time path.

Hit detection is a job followed by one guarded edit storing its results. Quantization
consumes persisted hits as a pure atomic edit. The GUI automatically detects absent
or stale hits before quantize/slice, with two history steps permitted. Slice at hits
means strength zero in the GUI; the command also quantizes slice positions.

## Initial scope

Linear slopes, aligned bundles, nonlooped audio, pitch-preserving Signalsmith mode.
Slice editing requires unwarped input. Musical beat-domain anchors, arbitrary-span
bundles, curved slopes, disk streaming and synthesized tail filling are deferred.

## Validation

Require canonical round trips and replay, one-step undo per edit, attack and phase
fixtures, source-coordinate edit regressions, deterministic renders and bit equality
between offline, callback and anticipative executors, including bridge playback.
Native CI on Linux, macOS and Windows is required before merge.
