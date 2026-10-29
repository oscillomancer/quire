# ADR-0054: Whole-chain channel freeze

**Status:** Accepted, 2026-10-28. Contracts: session-model.md and engine.md.
Amends ADR-0008's history/media lifetime contract; preserves ADR-0019's Console
9 boundary and ADR-0051's latency solve.

## Decision

A channel can replace its own items, take processing and entire insert chain
with a float32 pre-fader render. Original objects and serializable plugin states
remain in the model. The cache includes polarity/mono and auxiliary channels;
fader, pan, mute, their automation, outgoing sends and parent routing remain live.
Busses, masters and partial-chain freeze are excluded.

Capture uses the full graph schedule and private plugin instances. Frozen plans
retain the captured chain latency as virtual latency and read cached samples at
the output timeline shift. This saves plugin processing and memory while keeping
the initial compensation, mixer automation and Console 9 phase. It does not
promise reduced monitoring latency. No processing is inserted between a Console
9 encoder and decoder. Frozen media preparation uses MediaCache in one place.

The render action completes through a private replayable edit. Completion checks
project generation, branch, media directory, the current sound-dependency hash
and target scheduling quantities. Unrelated mixer or arrangement edits do not
invalidate a job; changed sound or latency scheduling does. History redo and
branch replay reuse the artifact, never rerender it. Freeze and unfreeze each
make one history step. Published media is retained like recorded media because
history, replay logs, branches, saved projects and autosaves can reference it.

Frozen edits reject changes to the captured sound, including pooled sources and
automation. Tempo, meter, sample rate and master-rate changes require unfreezing.
External audio/key/aux/MIDI input, recording/monitoring and non-unity master rate
are excluded. A finite reported tail plus a sample-counted silence window is
captured; infinite or unsettled tails refuse rather than silently truncate.

Original plugin instances are destroyed only on the control thread after both
executors release their plans. Dedicated bridges can exit; shared bridges retain
unrelated instances. Unfreeze restores stored settings/state, not unexported DSP
memory. A freeze is an offline performance: arbitrary random or block-sensitive
plugins, new seeks, repeated loops and stopped processing need not reproduce the
original evolving plugin output. Frozen realtime and offline playback share the
same reader and must agree. Other device rates resample the cache; equivalence
to newly processed unfrozen audio at a different rate is not promised.

## Consequences

Project schema 2.2 adds an optional track freeze descriptor referencing ordinary
project media. Old projects remain unfrozen. Older applications may drop the
new field and play the original chain. The GUI adds only context actions and
frozen indicators. Automatic published-media garbage collection, partial freezes,
buss freeze and editable baked content remain future work.
