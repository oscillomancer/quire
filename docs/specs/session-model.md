# Spec: session model, time model, versions and the project file (Phase 4)

**Status:** Draft for review (2026-09-26). Frozen before P4 implementation.
**Decision records:** ADR-0007 (bus), ADR-0008 (model, IDs, undo, branches),
ADR-0009 (formats), ADR-0010 (value domains).
**Libraries:** `libs/timeline` (`supple::time`: tempo/meter maps and time
conversion — pure, reusable by notation and the sandbox), `host/engine/model`
(`supple::model`: session types, edits, versions, branches, diff, project
file), commands in `host/engine/commands`.

This is the data the whole DAW is made of. Every object here is reachable by a
stable ID, editable only through commands, versioned, diffable and savable as
canonical JSON. The REAPER research (`docs/research/reaper-ux.md`) is the
workflow reference; where REAPER encodes structure implicitly (folder depth
by track order, receives by track index), Quire makes it explicit.

---

## 1. Object model

```
Session
├── project        settings, notes, render settings, next-id counters
├── tempo          tempo points + meter changes (the time map)
├── rulers         markers mk_*, regions rg_*, ruler lanes
├── tracks         tr_*  typed strips: channel | buss | master (ADR-0019);
│                        a forest rooted at the masters, tr_0 the main master
│   ├── chain      fx_* (ordered; containers nest), input chain
│   └── lanes      comp lanes (fixed lanes)
├── items          it_*  (on a track + lane), each with takes tk_*
├── sources        md_* audio media, mi_* MIDI sources (pooled or unique)
├── routes         sd_*  edges: track→track, track→hw out, hw in→track
├── envelopes      en_*  automation (targets any param), automation items ai_*
├── groups         gr_*  (not yet implemented; preserved as an unknown section)
└── blobs          content-addressed opaque data (plugin states, large arrays)
```

All collections are ID-keyed maps (ADR-0009). Ordered relations are explicit
arrays of IDs (`children`, `takes`, `chain`). Each object carries a `rev`
(revision stamp: the version number that last changed it) used for merge
conflict detection (§6.4).

### 1.1 IDs

`<kind>_<n>`, `n` from a per-kind counter in `project.next_ids`, never
reused within a lineage: `tr` track, `it` item, `tk` take, `md` media
source, `mi` MIDI source, `fx` plugin instance, `sd` route, `en` envelope,
`ai` automation item, `mk` marker, `rg` region, `tp` tempo point, `ts` meter
change, `gr` group, `ln` lane, `rj` render job. `tr_0` is the main master.
Events inside containers (MIDI notes, envelope points, stretch markers) are
not objects (ADR-0008).

### 1.2 Project

`name`, `title`, `author`, `notes`; `sample_rate` (the engine rate; media at
other rates is resampled, never converted on import); `ppq` default for new
MIDI (960); `timebase` default for new items (`time` | `beats` |
`beats_position`); `media_dir` (relative); `render` default settings (§1.10);
`next_ids`; `history_budget_bytes` (positive estimated bytes, default 1 GiB; §6.2);
`seed` (session randomness, e.g. dither); `console9` (bool,
default true: Console 9 summing on every strip; false keeps its gains and pan
law but runs the stages linear, ADR-0019 §6); `masters` (ordered master
ids, tr_0 first). *(`pan_law` was removed with ADR-0019.)*

### 1.3 Time map (`tempo`) — the contract lives in `libs/timeline`

- **Positions** are stored in one of two domains: **seconds** (double) or
  **beats** (double, quarter-note beats from project start). Every
  positioned object stores the domain its timebase implies. Conversion to
  samples happens only when compiling a render plan: `sample = round_half_even(seconds × sample_rate)`.
- **Tempo points** `tp_*`: `{beat, bpm, shape}` with `shape` `step` (tempo
  holds until the next point) or `ramp` (BPM changes linearly **in beats**
  to the next point). Seconds for a ramp segment from beat b0 (bpm v0) to b1
  (v1): `t(b) = t0 + (60 · (b1 − b0) / (v1 − v0)) · ln(v(b) / v0)` with
  `v(b) = v0 + (v1 − v0)(b − b0)/(b1 − b0)`; step segments are linear. The
  inverse is closed-form. The first point is at beat 0; there is always one.
- **Meter changes** `ts_*`: `{bar, numerator, denominator}` at bar starts
  (bar 1 is the first bar; the first change is at bar 1). Bars are derived:
  `beatOfBar(n)` sums bar lengths (`4·num/den` beats each).
- `libs/timeline` API: `TimeMap` (immutable, built from points/changes),
  `secondsAt(beat)`, `beatAt(seconds)`, `barBeat(beat) → {bar, beat,
  fraction}`, `beatOf(bar, beat)`, `bpmAt(beat)`, `meterAt(bar)`,
  `samplesAt(seconds, rate)`. Round-trip `beatAt(secondsAt(b))` within 1e-9
  beats; property tests over random maps; ramps tested against numeric
  integration.

### 1.4 Tracks (typed strips)

*(Rewritten 2026-09-30 for ADR-0019; the universal REAPER track is gone.)*
A track is a mixer **strip** of one of three types. Signal order and the
built-in Console 9 stages are implicit in the type (ADR-0019 §2); `chain`
holds only user inserts.

| type | summing input | items/record | children | built-in stages | fader/pan drive |
|---|---|---|---|---|---|
| `channel` | — (record/monitor inputs only) | yes | never | C9 Channel last | C9 Channel |
| `buss` | encoded strips | no | yes (folder) | C9 Buss first, C9 Channel last | C9 Channel |
| `master` | encoded strips | no | yes (folder) | C9 Buss first, plain gain/pan last | plain stage |

Fields:

| field | type | notes |
|---|---|---|
| `type` | `channel` \| `buss` \| `master` | default `channel`; tr_0 is always `master` |
| `name`, `color`, `notes` | string | |
| `parent` | track id or null | a buss or master; null exactly for masters (the roots) |
| `children` | [track id] | order among siblings; empty on channels |
| `channels` | int | 1-128 (busses and masters ≥ 2): 1/2 are the main pair (fader, pan, Console 9, meters, parent send); 3-128 are auxiliary channels that routes and plug-in pins address, never faded, panned or sent to the parent (ADR-0020); a multi-out instrument's outputs land there (ADR-0021). A mono channel is placed by its pan |
| `volume` | dB (−inf..+12) | the fader: C9 Channel Fader on channels/busses (0 dB = native unity; +12.04 is the native top, clamped to +12), plain gain on masters |
| `pan` | % −100..100 | C9 Channel Pan (sin/cos, −3 dB centre) on channels/busses; balance on masters (0 dB centre) |
| `mute`, `solo` | bool / enum (`off`, `on`, `in_place`, `defeat`) | mute ramps the fader to −inf before the encode |
| `parent_send` | `{enabled, channels: {from: [..], to: [..]}}` | the encoded output into the parent (no level of its own) |
| `record` | `{armed, input: route id or null, monitor: off|on|auto, mode: input|output|midi_overdub|midi_replace|...}` | channels only |
| `timebase` | enum or `project` | default for its items (channels) |
| `chain`, `input_chain` | [fx id] | §1.7; `input_chain` on channels only |
| `lanes` | `{mode: none|fixed|comp, lanes: [ln id], comp: …}` | channels only (Phase 5 UI; model now) |
| `locked`, `hidden_tcp`, `hidden_mixer`, `folder_compact` | bool / enum | UI state kept in the model because it is user intent, but tagged `ui: true` so diffs/merges can ignore it |

`volume`, `pan` and `mute` are automatable (§1.9). Width, trim and polarity
are not strip fields: insert a utility (Plumb) for them.

### 1.5 Items and takes

**Item** `it_*`: `track`, `lane` (null = main lane), `position` (in the
item's timebase domain), `length`, `timebase` (`track` = inherit), `loop`
(bool: loop the source), `mute`, `locked`, `gain` dB, `fade_in` /
`fade_out` `{length (s), shape: linear|slow|fast|slow_fast|fast_slow|cos|exp, curve %}`,
`crossfade` implicit when items overlap on a lane (auto-crossfade length from
the overlap), `takes` [tk id], `active_take`, `snap_offset`, `group`,
`color`, `notes`.

**Take** `tk_*`: `name`, `source` (md or mi id, or null = empty take),
`start_offset` (seconds into the source, or beats for MIDI),
`playrate` (0.01..100), `preserve_pitch` (bool), `pitch` (st), `pitch_mode`
(algorithm id; Phase 4 ships a Signalsmith-Stretch-based mode (MIT) and
`resample`), `gain` dB, `pan` %, `phase_invert`, `channel_mode`
(`normal|reverse_stereo|mono_mix|left|right|...`), `stretch_markers`
(array of `[source_pos_s, item_pos_s, slope]`), `take_markers` (array of
`[pos_s, name, color]`), `chain` (take FX), envelopes via §1.9.

### 1.6 Sources

- **Audio media** `md_*`: `{path (relative to media_dir when inside it),
  hash (SHA-256 of file bytes), format, sample_rate, channels, frames}`.
  Decoding and resampling to the engine rate are cached by the engine keyed
  by `(hash, rate)`; peak pyramids are cached outside the project (never in
  diffs). Recording sources may initially have an empty hash. A background
  completion replaces matching media metadata in stored sessions without a
  version/undo step or revision change; existing reader leases remain immutable
  and retain their versions. An undone/deleted take drops its completion.
  Missing files are reported, never fatal.
- **MIDI** `mi_*`: `{ppq, length (ticks), notes, controls, sysex, pooled:
  bool}` — a `midi::Sequence` (notation spec Part A). Several takes may
  reference one pooled source (edits affect all); unpooled sources belong to
  one take. The notation is a *view* of this data (`midi.get
  {format: "notation"}` / `midi.write`), never the storage.

### 1.7 FX chains and instances

**Instance** `fx_*`: `plugin` `{format: clap|vst3|lv2, id, name, vendor,
version}`, `label` (instance name), `bypass` (processing skipped, latency
kept), `offline` (unloaded; audio passes through), `wet` % (dry/wet),
`isolation` (`inherit|in_process|shared_bridge|dedicated`), `oversampling`
(1, 2, 4, 8), `pins` `{in: [l, r], out: [l, r], key: [l, r]}` — the track
channels (1-based) a track-chain plug-in reads, writes and keys from (a
detector on 3/4); one entry feeds both pins (in, key) or takes the left output
(out); empty: 1/2 in and out, the key from key routes only (ADR-0020); `out`
past its first two entries names a track channel per auxiliary output channel
of a multi-out plug-in, in order (0: unconnected; `fx.add` sets them for an
instrument, growing the track, ADR-0021),
`parallel` (bool: run in
parallel with the previous instance in the chain, outputs summed), `pdc`
(`compensate|ignore`), `auto_bypass_on_silence` (bool), `container` (for
`plugin.format == "container"`: a nested `chain` array plus its own pins),
**state**: for plugins with the semantic extension, `patch` (canonical patch
JSON, diffable); otherwise `state` = blob ref (content hash) plus `params`
(last known CLAP values by param id, for display/diff only — the blob is
authoritative).

Chains are arrays of fx ids on tracks (`chain`, `input_chain`), takes
(`chain`) and the master's monitoring chain (`project.monitor_chain`, not
rendered).

### 1.8 Routes

`sd_*` edges, stored at top level (not inside tracks):

| field | notes |
|---|---|
| `from` | `{track: id}` or `{hw_input: {channels: [..]}}` or `{midi_input: device}` |
| `to` | `{track: id}`, `{fx: id}` (a plug-in's key, ADR-0018) or `{hw_output: {channels: [..]}}` |
| `tap` | `post_fader` (default), `pre_fader` (post-FX), `pre_fx` |
| `volume` dB, `pan` % or `follow` (default: the strip's pan), `mute`, `phase_invert`, `mono` | into a buss/master: realized by the send's own C9 Channel (ADR-0019 §3) |
| `channels` | `{from: [..], to: [..]}` channel mapping, 1-based up to each strip's channels (e.g. [1,2] → [3,4] for a detector on 3/4). Main pairs map the encoded send; pairs touching auxiliary channels are linear links at the route's level, mute and polarity (ADR-0020) |
| `midi` | `{from_channel: all|1..16, to_channel: original|1..16}` or null (audio only) |

Folder/master sends are the tracks' `parent_send`, not routes. Record inputs
are routes from `hw_input`/`midi_input` referenced by `track.record.input`.
Cycles are rejected at edit time (`route.cycle`, naming the cycle).
ADR-0019 §4 rules, rejected at edit time with `route.strip_type` (and a hint
naming the allowed alternative): audio into a channel only from `hw_input`;
audio into a buss/master only from strips; `hw_output` only from masters; key
routes from any tap of any strip.

### 1.9 Envelopes and automation items

`en_*`: `target` — one of `{track: id, param: volume|pan|mute}`,
`{route: id, param: volume|pan|mute}`, `{fx: id, param: <key or CLAP id>}`,
`{take: id, param: volume|pan|pitch|mute}`, `{tempo: true}` (the tempo map is
edited through `tempo.*`, not envelopes); `mode`
(`trim|read|touch|latch|latch_preview|write`); `active`, `armed`,
`visible`; `points`: array of `[pos, value, shape, tension]` in the
envelope's time domain, sorted, with `shape` `linear|square|slow|fast_start|fast_end|bezier`;
`items`: [ai id], derived at serialization from the authoritative
`automation_items` map in numeric ID order. It is not a model field. On load,
stale or mismatched lists are ignored with a `project.derived_items` warning.

Values are in the target's physical unit (dB, %, Hz for stock plugin
params, CLAP value for third-party params); interpolation happens in the
target's normalized domain (ADR-0010). Automation items `ai_*` (top level,
`automation_items`, written only when there are any): `{envelope, position,
length, source_length (beats), loop, pool (0: none; items of one pool share
points and source_length), points (beats from the item's start), baseline,
amplitude, name}`. An item replaces the envelope over its span: its points
repeat every `source_length` while `loop`, values play as `baseline +
amplitude × (value − baseline)`, a later item cuts an earlier one it
overlaps, and the envelope steps at each edge (gap log #28). Take envelopes
(`{take: id, param}`) use item-relative beats and move with the item; they
take no automation items. Expansion is bounded to 100,000 repetitions/points
per envelope; over-budget edits and files refuse with `automation.budget`.
Copied takes clone their FX, envelopes and automation items with new IDs;
automation items retain their pool. Splits insert the curve value at the cut
and retain the segment shape on both sides. MIDI overdub uses the take's play rate.

### 1.10 Rulers and render settings

Markers `mk_*` `{pos, timebase, name, color, lane}`; regions `rg_*` `{start, end,
timebase, name, color, lane}`; ruler lanes `{id, name}`.

Render settings (defaults for `render.*`): `format` (wav/flac, bit depth,
dither), `sample_rate`, `channels`, `normalize` (`none` | `{mode: lufs_i|
lufs_s|lufs_m|peak|true_peak|rms, target, limit_true_peak_db?}`), `tail_ms`,
`pattern` (file-name pattern, §7.3), `secondary` format, `matrix`
(`{rg id or "project": [track/master ids, e.g. "tr_0"]}`).

### 1.11 Blobs

`blobs`: map from `sha256:<hex>` to `{size, encoding: "base64", data}` in the
project file (or sidecar files beyond 1 MiB, §5.3). Immutable,
deduplicated, reference-counted across versions.

---

## 2. In-memory representation

- Every object type is an immutable C++ value struct; collections are
  `ImmutableMap<Id, std::shared_ptr<const T>>` (a sorted vector for ≤ 256
  entries, a persistent HAMT beyond — behind one interface so it can start
  as a sorted vector and upgrade without touching callers; ADR-0008 allows
  `immer` if needed). Large arrays (MIDI events, envelope points) are chunked
  (`ChunkedArray<T>`, 4096 elements per chunk) so editing one note copies one
  chunk.
- `Session` holds the root pointers; edits produce a new `Session` sharing
  everything untouched. Readers on any thread hold a `std::shared_ptr<const
  Session>`; nothing is ever mutated in place.
- `EditTxn` is the only way to build a new version: typed setters that
  copy-on-write the path, assign IDs from counters, stamp `rev`, and record a
  per-object change list for events and diffs.

## 3. Validation invariants (checked after every edit, in debug builds after every command; always before save)

The track tree is a forest rooted at the masters (`project.masters`, tr_0
first, all of type `master`, parent null); every other track's parent is a
buss or master; channels have no children; items, lanes, record settings and
`input_chain` only on channels; routes obey ADR-0019 §4; every referenced ID exists; items'
tracks/lanes exist; `active_take` ∈ `takes`; sources referenced by takes
exist (or are marked missing); no route cycles (including parent sends);
envelope targets exist; points sorted; chains reference fx that belong to
exactly one chain; counters exceed every used ID; tempo map has a point at
beat 0 and meters at bar starts. A violation is a bug (`model.invariant`),
never a user error — user errors are caught by command validation first.

## 4. Diff and describe

- `diff(Session a, Session b) → [{op: add|remove|update, id, kind, fields:
  [{field, before, after}]}]` — object-level, deterministic order (kind
  order as in §1, then ID). Large arrays diff as summaries (`notes: 3 added,
  1 changed`) with an optional detailed form. Used by `session.changed`
  events, branch reviews and the undo history display.
- `describe(session, scope, detail)` → compact text for LLMs, e.g.

```
Project "Demo" · 48 kHz · 120 BPM 4/4 · 16 bars (0:32.0)
tr_1 Drums [buss] → master
  tr_2 Kick   vol −3.0 dB · fx: fx_2 EQ (4 bands), fx_3 Comp (−4 dB GR@1:1…)
              it_4 bars 1–17 "kick loop" (audio md_1, 1.00×)
  tr_3 Snare  it_5 bars 1–17 (MIDI mi_2, 64 notes)
tr_6 Bass → master · fx: fx_7 Orrery "Round Sub", fx_8 EQ
routes: sd_1 tr_2 → tr_6 ch 1/2→3/4 pre-fader −6 dB (sidechain)
markers: mk_1 "Verse" bar 1 · regions: rg_1 "Chorus" bars 9–17
```

## 5. Project file (`supple.project/2.0`)

### 5.1 Shape

*(Schema `supple.project/2.0` since ADR-0019: strip `type`,
`project.console9`, `project.masters`; strip `width`/`trim`/`phase_invert`
removed. 1.x files migrate as ADR-0019 §11 says, with warnings.)*

The loader accepts 1.x and 2.x documents and saves 2.0. Migration allocates
child channel IDs in track-ID order, inserts each child first, and moves the
source items and record/input FX there while keeping user inserts on the
buss. Hardware/MIDI record routes follow the new child. Legacy numeric send
pan remains numeric (including the old default centre); new sends default to
`follow`. `project.open` includes nonempty load `warnings` as structured
`{code, message, path}` entries, including the change to Console 9 staging.
Masters are ordered by `project.masters`, independently of processing order;
`track.list` and `session.describe` visit every root in that order.

Canonical JSON (ADR-0009) with members in this order: `schema`, `project`,
`tempo` (`points`, `meters`), `rulers` (`markers`, `regions`), `tracks`,
`items`, `takes`, `fx`, `sources` (`media`, `midi`), `routes`, `envelopes`,
`automation_items` (when present), `blobs`, then preserved unknown top-level
sections and `extensions`. `groups` / `gr_*` are not yet implemented; a
`groups` section is preserved with a warning. `ui` belongs to the sidecar. *(Amended 2026-09-26: `fx` — plugin instances are
ID-keyed objects like the rest; chains reference them by id.)* Members equal
to their defaults are omitted (the schema fills them in on load). Objects keyed by ID in ID order
(numeric by kind). Compact array forms for bulk data:

- MIDI notes: `[[start, length, channel, pitch, velocity, off_velocity], …]`
  (ticks; channel 1-based); controls `[[tick, kind, channel, number, value], …]`
  with `kind` `cc|program|pressure|poly|bend`; sysex `[[tick, "F0 … F7"], …]`.
- Envelope points `[[pos, value, shape, tension], …]`.

These arrays are the one place canonical form prints one inner array per
line (scalar arrays of arrays: each inner array on one line, outer array one
element per line) — a writer option `compactRows` in core-json. *(Requires a
small core-json addition; tracked as P4.1.)*

### 5.2 Compatibility policy

Project exception to ADR-0009: known sections/fields reject wrong types or
shapes with a field path for every minor. Unknown fields within known objects
are dropped with warnings listing their paths. Unknown top-level sections and
an explicit top-level `extensions` object are preserved verbatim on save.
A newer minor loads with a version warning; older majors migrate and newer
majors refuse. A project `ui` member is ignored with a warning pointing to
`<name>.ui.json`. `groups` is preserved with a warning that track groups are
not implemented. The parser is
fuzzed (`project.fuzz`). Unknown plugin formats or missing plugins load as
placeholder instances that keep their state untouched and pass audio
through (the session survives; `project.load` reports them).

### 5.3 Files on disk

The extension derives from the product slug: `<name>.<slug>` (e.g.
`demo.quire`), plus an optional sidecar directory
`<name>.<slug>-data/` for blobs over 1 MiB (`<hash>.bin`) and autosaves.
UI state lives separately in `<name>.ui.json` (gui.md §1.2), never in the
project document. Saves are atomic (write temp, fsync, rename). Autosave every 60 s to
`<name>.<slug>-data/autosave-<n>.json` (keep 5). History is persisted only
on request (ADR-0008 clarification).

The engine app writes the autosaves from its service tick (engine.md §6),
only when the head changed since the last one and 60 s have passed, and
`<n>` counts up from the highest present. `project.open` of a file that has
a newer autosave next to it reports `autosave_available` and loads nothing
from it; opening the autosave file itself is how it comes back.

## 6. Versions, undo, branches

### 6.1 Version store

`VersionStore`: `Version {id (monotonic u64), parents [1–2], session
(shared_ptr), txn {label, origin, invocations (resolved, for replay),
created_ids}, time}`. Branch refs: `main`, `agent/<agent>/<topic>`, user
branches. Each branch has a head and an undo cursor.

### 6.2 Undo / redo

Per branch, linear. `history.undo` moves the head to its parent (the
undone version stays as the branch's redo target until a new edit);
`history.redo` moves forward. `project.set {history_budget_bytes}` changes the
persisted estimated history byte budget (default 1 GiB, positive integer).
Above it, oldest eligible linear steps on each branch combine into one
`earlier history` step; intermediate snapshots are released and undo/redo crosses
the combined step as one operation. Shared MIDI/blob/object/collection payloads
are counted once. Leases, branch bases/heads, redo targets and fork/merge
boundaries survive; required data/replay logs may exceed the budget.
`history.list` reports `truncated`, `retained_bytes`, `budget_bytes`, and
`squashed_steps` per entry. This does not persist undo history into project files.
Coalescing: consecutive edits with the same `coalesce_key` (e.g. one fader
gesture, `fx.param.set` on the same param within one gesture) replace the
head instead of stacking.

### 6.3 Branch operations

`branch.create {from: "main", name}`, `branch.list`, `branch.switch
{name, mode: live|offline}` (live: engine reconciles to that head),
`branch.diff {a, b}`, `branch.render-ab {a, b, range, ...}` (offline renders
of both + analysis comparison, returning two render ids for sample-aligned
A/B playback), `branch.merge {from, into: "main"}`, `branch.delete`.

### 6.4 Merge by replay

If `into` has not moved since the fork: fast-forward. Otherwise replay the
source branch's transactions (in order) onto `into`'s head:

1. For each logged invocation, rewrite IDs through the remap table (IDs the
   branch created that collide with IDs created on `into` since the fork get
   fresh IDs; schemas identify ID-typed fields: only strings, and object
   keys, whose argument-schema pattern is an ID pattern such as `^tr_[0-9]+$`
   are rewritten — a track *named* "tr_1" keeps its name). Coalesced edits
   never erase a version that another branch's head or base still refers to.
2. Re-validate and apply. A failure is a **conflict** of kind `invalid`.
3. Before applying, compare the `rev` of every object the invocation touches
   against the fork point: if `into` changed that object since the fork,
   it is a **conflict** of kind `concurrent_edit`.
4. On conflicts, the merge stops without changing `into` and returns the list
   `[{step, invocation, kind, object, into_value, branch_value}]`. The human
   (or agent, on request) resolves by editing either branch and merging
   again, or by `branch.merge {..., strategy: "take_branch" | "keep_main"}`
   per conflict.

### 6.5 Engine reconciliation

`RenderPlan` compilation (engine spec) diffs the previous compiled version
against the new one: unchanged plugin instances are reused as-is; instances
whose `patch`/`state` changed get it applied (`set_patch`/`state.load` on
the main thread); new instances are created and activated off the RT path;
removed instances are deactivated and destroyed after the audio thread has
released the old plan.

---

## 7. Command catalogue (Phase 4 scope)

Schemas are written per command in code (ADR-0007); this is the inventory
and naming. All `edit` unless marked (q) query or (a) action.

- **project**: `project.new` (a), `project.open` (a), `project.save` (a),
  `project.get` (q), `project.set`, `project.describe` (q).
- **session**: `session.describe {scope, detail}` (q), `session.query
  {kind, filter, fields}` (q), `session.get {id, fields}` (q).
- **tempo/meter**: `tempo.set {points}`, `tempo.add`, `tempo.remove`,
  `timesig.set {bar, numerator, denominator}`, `timesig.remove`,
  `time.convert {from, to, value}` (q).
- **tracks**: `track.add {name, type: channel|buss|master, parent, index, channels, template?}`,
  `track.remove`, `track.move {parent, index}`, `track.set {fields}`,
  `track.duplicate`, `track.list` (q).
- **items/takes**: `item.add {track, position, length, source|midi|notation}`,
  `item.remove`, `item.move`, `item.set`, `item.split {at}`,
  `item.duplicate`, `item.glue` (a+edit), `take.add`, `take.remove`,
  `take.set`, `take.activate`.
- **MIDI**: `midi.write {item | {track, at}, notation, replace: bool}`,
  `midi.get {item, format: notation|events}` (q), `midi.edit {item, ops:
  [{select: {pitch, range, channel}, set|shift|transpose|quantize|velocity}]}`,
  `midi.quantize`, `midi.humanize {seed}`, `midi.import {file, track}`,
  `midi.export` (a).
- **FX**: `fx.add {chain_owner, plugin, index, patch?}`, `fx.remove`,
  `fx.move`, `fx.copy {fx, track, index?, chain?}` (live settings), `fx.set {bypass, offline, wet, isolation, oversampling,
  parallel, pins, …}`, `fx.param.set {fx, values: {key|id: value}}`,
  `fx.param.get` (q), `fx.patch.get` (q), `fx.patch.set`, `fx.patch.merge`,
  `fx.describe` (q), `fx.preset.save` (a), `fx.preset.load`,
  `fx.state.capture` (internal, from plugin notifications), `plugin.list`
  (q), `plugin.scan` (a).
- **routes**: `route.add {from, to, tap, channels, …}`, `route.remove`,
  `route.set`, `route.list` (q).
- **automation**: `env.add {target}`, `env.remove`, `env.set {mode, active,
  …}`, `env.points.set {env, points, range?}`, `env.points.add`,
  `env.points.remove {range}`, `env.add {take, param}` (take envelopes);
  automation items `autoitem.add {env, position|bar, length, points?,
  source_length, loop}` (no points: captures what plays there),
  `autoitem.set` (points and source_length reach the pool),
  `autoitem.duplicate {pooled}`, `autoitem.unpool`, `autoitem.remove`,
  `autoitem.list` (q).
- **rulers**: `marker.add/remove/set`, `region.add/remove/set`.
- **transport** (a): `transport.play`, `transport.stop`, `transport.pause`,
  `transport.record`, `transport.seek {pos}`, `transport.loop {range}`,
  `transport.punch {start, end}`, `transport.metronome {count_in_bars,
  count_in_playback, …}` (a count-in before recording), `transport.get` (q).
- **render** (a): `render.mix {master}` (default tr_0), `render.stems`, `render.regions` (matrix),
  `render.items`, `render.dry-run` (q: resolved file list), `render.job.*`;
  `format` (wav|flac), `secondary` (a second file in another format/bit depth
  from the same render); `render.queue.add/list/remove/run` (renders queued
  with their arguments, run in order).
- **analysis** (q): `analyze.render {render}`, `analyze.track {track, range}`
  (renders the track's output offline, then analyzes), `analyze.compare`,
  `analyze.masking {masker_track, maskee_track, range}`.
- **history/branches**: `history.undo`, `history.redo`, `history.list` (q),
  `branch.*` (§6.3).
- **scripting**: `script.run {code, name?, budget_ms?, args?}` (a) — Lua on the
  model thread in a sandbox (ADR-0006, `libs/lua-bus`), its calls recorded
  under `lua:<name>`; `q.batch` inside is one transaction and `q.on` subscribes
  to the events below. Not transactional itself: undo its edits with
  `history.undo`.
- **meta**: `batch`, `registry.*`.

### 7.3 Render file-name patterns

A closed set of `{variable}` tokens with explicit formatting args instead of
REAPER's `$`-mini-language: `{project}`, `{track}`, `{track_number:02}`,
`{folder_path}`, `{region}`, `{region_number:02}`, `{marker}`,
`{item}`, `{take}`, `{start:beats|seconds|timecode}`, `{format}`,
`{sample_rate}`, `{bit_depth}`, `{date:YYYY-MM-DD}`, `{time:HH-mm-ss}`,
`{counter:03}`. Filters: `|lower`, `|upper`, `|replace(" ","_")`,
`|max(8)`. `render.dry-run` returns the resolved list. Empty tokens drop an
adjacent separator (`-`, `_`, `.`, space), as REAPER does.

---

## 8. Tests required (P4)

timeline property tests (§1.3); model edit tests per command; invariant
checker; diff determinism; describe goldens; project file round trip
(byte-identical) and fuzzing; version store: undo/redo round trips under
random command sequences (fuzzed: random valid command streams, then undo
all → initial state `identical`, redo all → final state); branch merge
fast-forward, replay with ID remapping, both conflict kinds; memory-bound
eviction; blob dedup.

### Follow-up contracts (2026-10-04)

- Undo/redo reloads the active engine plan and emits `session.changed` as
  `{version, branch}`, like an edit. Per-lineage ID high-water counters survive
  undo; adding after undo cannot reuse an ID. Independent branches may allocate
  overlapping IDs; merge remaps each creation before replaying the next command,
  including commands in the same batch.
- Project fields are represented in changes/diffs by the singleton `project`.
  Concurrent changes to the same project field conflict only when their new
  values differ; disjoint fields replay normally.
- Each invocation inside a batch has a savepoint. A caught failure restores
  that invocation's model/history/event changes while successful siblings remain.
- `project.new/open` stops and finishes recording, cancels and joins project jobs,
  then increments `project.get.generation` and resets session caches/scripts.
  Device and render-ahead settings persist. Deferred completions check generation;
  stale completions emit an event and never mutate the replacement project.
- First save stages and hashes scratch-media copies, publishes the media, then
  atomically writes the project. A failure removes files created by that attempt;
  scratch originals and the unsaved project identity remain intact.
- Render completion/import is bound to the originating project generation and
  current branch. A stale completion preserves files and reports
  `render.stale_target` in the job result; no items are imported.
  A job cancelled during project replacement keeps its cancelled status and
  receives the stale result when its queued completion is serviced.
- `track.add` and `track.set` accept 1..8 channels; busses/masters require at least 2.
  `fx.param.set` keeps semantic parameter units and also supports plain CLAP/VST3
  parameter names/IDs, declared ranges, writable flags and plugin text conversion.
  Plain parameter edits save processor state after applying queued values on a
  private probe. Latency probes restore patch/state/params before measuring.
- Ruler lists include `timebase`; descriptions map seconds through the time map.
  MIDI edits reject non-finite/oversized tick conversions and arithmetic before
  rounding, including extreme deltas and impractically small quantization grids.

### Numeric positions and expansion budgets (2026-10-04)

Every edit and project load validates original numbers and derived ends, seconds,
source offsets and sample products. Positions must fit signed-int bar addressing
and signed-64 samples at the project/device/render rate, with space for block, render-ahead and
PDC additions. Take playback/pitch transforms must have finite positive speeds
and representable source/variant frame lengths. MIDI ticks and note-end sums
are checked before use; materialized MIDI expansion is limited to 1,000,000
events/passes per track to bound planning memory and keep note IDs in int32.
Automation retains its 100,000 expanded-point budget and validates finite scaled
curves. In-memory command renders use the engine’s existing 2^28-frame capacity
at the project and requested output rates, with block headroom; this is a work
budget, not a new duration policy.
