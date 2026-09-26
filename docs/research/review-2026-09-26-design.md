# Design review — architecture, ADR-0001–0011, core-json/core-schema, MIDI/notation

2026-09-26. Read-only review of the main checkout and `/home/nate/Projects/build-prompt-llm-native-daw.md`. No repository files were edited. Findings below concern contracts and feasibility, not the quality of unfinished implementations. Severity is about the cost of proceeding under the current text. Lines refer to the documents as read today.

## Critical — resolve before freezing the two implementations

### C1. The notation round-trip law is false for valid `Sequence::length` values

`docs/specs/notation.md:49` permits `length` in arbitrary ticks and :410–412 requires `compile(decompile(s)).sequence == s` for **any** canonicalized sequence. But notation :131 defines `length 8` in **bars**, and :415–417 says the decompiler writes `length N` when needed. Counterexample: ppq 960, 4/4, no events, `length=1000` ticks. No integer number of bars encodes 1000; neither an unwritten partial bar nor a last event can supply the length. The build prompt :138 requires lossless MIDI→text→MIDI for everything represented. Add `length Nt` or an exact tick length field, define empty-item behavior, and test arbitrary lengths including a length after the final event but inside a bar.

### C2. Ordered same-tick MIDI controls cannot pass through a lane

The MIDI contract at `notation.md:65–69` says controls with identical `(tick, kind, channel, number)` **keep relative order** because the last value wins. The notation contract at :314 and :404 requires lane points in increasing time and `lane_order` rejects repeats; :438–441 decompiles one lane per key. A sequence with CC74 values 20 then 110 at tick 0 is valid and ordered in the model, but no lane text can contain both. The :410–412 arbitrary-sequence law and :455–457 random-control property test are impossible. Permit nondecreasing positions with preserved input order (and specify duplicate ramp semantics), add a raw-events escape hatch, or narrow the model/law explicitly. Do not silently deduplicate because the model says order is meaningful.

### C3. The merge-patch API promises an impossible result for general JSON

`core-json.md:178–180` promises `createMergePatch(from,to)` with `mergePatch(from,p)==to`, while :192–194 explicitly admits RFC 7396 cannot set an object member to `null`; :438–439 still requires the property for **random documents**. `from={}`, `to={"x":null}` has no RFC 7396 patch. The restriction “documents never use null as meaningful value” does not restrict `json::Value`, the advertised function, or the random JSON test. Scope the function/result to a validated no-null document domain and return an error otherwise, or use a different patch type. The current `Value` return type cannot report unrepresentability.

### C4. The isolated-plugin synchronization plan violates absolute RT rules as written

The prompt :93 says the audio thread “never ... blocks”; `ARCHITECTURE.md:72–81, 93–96` says neither audio nor RT workers block and the RT thread consumes lock-free buffers. Yet `ARCHITECTURE.md:139–141` says bridged plugin RT threads are “pinned to the engine's cycle via shared-memory audio buffers and futex signalling.” A futex wait in the engine callback blocks; futex wake is a syscall, contrary to ADR-0003:26–28's `SUPPLE_RT`/RealtimeSanitizer syscall rule. A synchronous bridge must also bound its wait for a stalled plugin to meet the prompt's 64-frame benchmark and crash survival. Specify a nonblocking, bounded deadline and fallback (possibly one-block latency), exact buffer ownership and crash detection on a control thread. Identify which thread, if any, is allowed to call futex and how the audio callback proceeds if the bridge misses its deadline.

### C5. ExternalProject offline plugin builds have no clean-clone source plan

Prompt :121 requires “builds from a clean clone with one command”; ADR-0003:32–36 and :66 says no network at configure and dependencies vendored/pinned. ADR-0011:25–33 instead supplies `FETCHCONTENT_SOURCE_DIR_*`, `CLAP_SDK_ROOT` and `VST3_SDK_ROOT` from “one shared, pinned dependency cache,” citing a configure that used **preexisting template build trees**. A clean clone has none of those paths; the template's own FetchContent/CPM then needs network. Declare exactly how every plugin source and transitive SDK is populated from committed `third_party/` trees or a reproducible pre-build bootstrap, and make a clean `/tmp` clone/build test the gate. If plugins are excluded from the default clean build, state that exception explicitly; currently `SUPPLE_BUILD_PLUGINS` defaults ON.

## Major — contract gaps and Phase 4/5 traps

### M1. Template-derived plugins are C++17 unless the template is changed

Prompt :79 explicitly requires C++20 for plugins; ADR-0003:15 says the language is C++20. ADR-0011:20–33 says copies keep the template infrastructure/CMake shape and build as independent ExternalProjects. The actual template sets `CMAKE_CXX_STANDARD 17` (`template/CMakeLists.txt:28`) and GUI target `CXX_STANDARD 17` (`template/plugin/gui/CMakeLists.txt:264`), overriding a parent build. C++20 `core-params` headers will fail in such a plugin if they use C++20 features. Decide and approve a template patch or a product-copy override that really sets all plugin targets to 20; add a compile test through an ExternalProject. This is a direct requirement mismatch, not a cosmetic compiler setting.

### M2. ADR-0011 simultaneously freezes and rewrites wrapper/state behavior

ADR-0011:23–25 says “Infrastructure (wrapper, GUI stack, host_contract, probes) stays as the template ships it.” Its :35–54 requires `paramInfo`/text helpers to use `core-params`, a new extension from `plugGetExtension`, and replacing binary CLAP state with core-patch JSON plus transactional loading. Those are wrapper/state changes. Prompt :72–74 requires general template improvements to go through Nate's review queue rather than silently fork behavior; ADR-0011:56–59 also says to propose them. Spell out a patch/approval boundary and timing before P2.3: which changes are product-specific, which are template patches, and whether work pauses on the latter. Otherwise the first stock plugin cannot implement the extension while obeying the “stays as shipped” clause.

### M3. JSON Schema export does not describe the decoder's accepted unit strings

`core-json.md:336–339` validates strings by parsing compatible units and applying numeric ranges. But :412–422 exports a numeric field that accepts unit strings as `type:["number","string"]`, with `x-unit-strings` as a nonstandard hint. A normal JSON Schema/MCP validator accepts `"garbage"` and `"-999 kHz"` because numeric `minimum`/`maximum` do not constrain strings. This conflicts with :205–207's “one schema declaration drives ... validation ... and exports JSON Schema ... so ... never disagree,” and ADR-0007:35–38's generated tool schemas. Either export a real string branch with an enforceable grammar/range where possible, restrict MCP input to canonical numeric values, or explicitly say exported schemas are only an approximation and server-side validation remains authoritative.

### M4. `core-schema` has no versioned-document entry point despite ADR-0009

ADR-0009:20–31 requires the first `schema` member, version checks, newer-minor unknown-member warnings, and older-major migration before validation. `core-json.md:310–324` exposes generic `Schema<T>::decode` with default `UnknownMembers::Error`, but no envelope/version parser, migration registry, or rule choosing `UnknownMembers::Ignore` from the document's version. `jsonSchema()` at :412–422 likewise cannot convey the dynamic older/newer-minor policy. This will be duplicated differently by core-patch, project files and reports. Freeze a reusable `decodeDocument(schema id/current version/migrations)` contract before they build on it; say whether the `schema` field is part of `T` and how warnings survive RPC/file boundaries.

### M5. MIDI SMF import discards information a DAW may need

`notation.md:58–71` advertises SMF format 0/1 reading but returns one merged `Sequence` and `SmfInfo` with only one optional meter and bpm; other meta events are skipped. A format-1 file can have multiple tracks and tempo/meter changes. The prompt :207–211 requires a tempo map, time signatures and MIDI items/takes; flattening these files silently loses arrangement and timing structure. Either scope this reader to a single MIDI clip and return explicit warnings about dropped structure, or provide an SMF document model with per-track events and tempo/meter maps for DAW import. Do not call a format-1 read/write round trip lossless under the current shape.

### M6. The model/UI thread is a single latency hotspot

`ARCHITECTURE.md:83–86` makes the model thread the sole session writer and, in the GUI, “the UI thread”; it also runs the command bus, compiles `RenderPlan`s, services CLAP main-thread callbacks and owns plugin GUIs. ADR-0006:18–27 adds Lua scripts there. Prompt :231–245 requires a responsive GUI with large sessions and fast interaction. A large graph rebuild, plugin state capture/serialization, Lua callback or burst of agent commands can freeze input and editor timers. Preserve the one-writer rule, but separate UI event/render thread from model work or make plan compilation and long commands asynchronous with ordered commits; define a budget for synchronous model/UI work before Phase 4.

### M7. Unbounded immutable history and opaque plugin state threaten memory

ADR-0008:17–26 shares unchanged nodes and large payloads but retains versions through undo, redo and branches; :42–47 captures opaque plugin state as versions. It does not define pruning, checkpointing, blob deduplication, branch retention or memory limits. A 10 MB opaque preset captured repeatedly or dense automation gesture commits can consume GB while the app stays open. This is cheap to fix in the model contract now: bound undo history by configurable bytes/versions, deduplicate immutable blobs, release unreachable versions and state what survives a project save. It matters to the prompt's lightweight/large-session UX (:41, :231–245).

### M8. `writeSmf` cannot satisfy file-boundary error policy on invalid input

`notation.md:47–52` exposes mutable fields with documented constraints (nonnegative length/start, ppq positive in practice, channel/pitch/value ranges, valid F0…F7 sysex), but :61 returns a plain byte vector from `writeSmf` with no validation/error channel. `AGENTS.md` requires structured errors across file-format boundaries; ADR-0003:15–18 says library errors use `Result<T>`. Change it to `Result<std::vector<uint8_t>>` with structured path/hint or explicitly validate a separate immutable `Sequence` type before it can reach the writer. Add overflow checks for start+length and SMF delta-time encoding.

### M9. Decompiler chord grouping can lose note-off velocity

`notation.md:425–434` groups notes with “identical start, length and velocities” into one chord, while :199–202 and :221–222 provide only one shared note-off velocity for a chord. If “velocities” means note-on velocity alone, two simultaneous notes with different `offVelocity` lose data. Even if it means both, the spec should say so and split into separate voices when note-off velocities differ. Also :433–434 says `/MM` is emitted when off-velocity is nonzero, but grammar :164 puts `/MM` **inside** `:NN/MM`; the decompiler must print the on-velocity even when it equals the voice default. This is a concrete round-trip case for the property test.

### M10. Pickups and mid-bar MIDI items lack a defined time origin

`notation.md:145–149` makes bar 1 relative to item start and demands full bar sums; `CompileContext::meterAt(int bar)` at :369–371 has no item start position or in-bar offset. Prompt :207–211 requires MIDI items on a sample-accurate musical timeline, and real pickups/items can start mid-bar. The notation can fake a pickup by filling earlier beats with rests, but then the MIDI item starts at the previous bar boundary. Define whether item notation is always bar-aligned, whether a partial first/last bar is legal, and how `meterAt` maps local bars to project bars. The melody trial below exposes this.

### M11. Meter-change and PPQ validation is underdefined

`notation.md:122–128, :353–359` permits standalone meter changes “inside a voice” that apply to every voice, but gives no syntax for the effective bar or rule for another voice that skips that bar. `Meter` only says denominator is a power of two (:45), and `ppq` is an unrestricted `int` (:48); a zero/negative PPQ or a meter whose bar length is fractional in ticks makes the exact-bar law :149 impossible or causes division by zero. Define effective-bar syntax globally, bounds/positive requirements and a `tick_fraction` rule for meter lengths. Give a multi-voice meter-change example.

### M12. `Value` equality hides differences that canonical text and diffs care about

`core-json.md:84–86` makes `Int(1)==Double(1.0)` and ignores object member order. :161–163 requires parse/write to retain **types and member order**, yet a test using `operator==` cannot prove that invariant. :186–189 says `diff` treats numerically equal numbers as equal, so `{"x":1}`→`{"x":1.0}` has no change even though canonical bytes and the JSON kind change. Provide a separate strict structural equality for serialization/property tests and decide whether diff/merge should preserve type/order changes for file formats. Also specify how `asInt()` handles integral doubles outside int64 to honor :67–71's “UB-free defaults.”

### M13. Subscriptions and model-to-audio queues have no overflow recovery policy

ADR-0007:60–63 says “Nobody polls” and clients subscribe to `session.changed`; `ARCHITECTURE.md:93–96` says bounded SPSC queues drop the newest item on overflow, including “plugin output events and param changes.” A dropped param/state change can leave the session model, undo log and actual plugin state divergent; a dropped `session.changed` leaves a UI/MCP subscriber stale forever if polling is forbidden. Define sequence numbers, overflow/resync snapshots, and distinguish lossy telemetry from reliable state changes. This is needed for the prompt's one command bus, correct undo including plugin state, and event-driven clients (:89–93, :214–216).

### M14. Runtime plugin latency changes need an engine transition contract

Prompt :205–207 requires PDC across arbitrary routing “including latency changes at runtime.” `ARCHITECTURE.md:75–85` and ADR-0008:75–79 only say a version is compiled into an immutable plan and atomically swapped; ADR-0004:19–23 delivers backend changes to the control thread. CLAP latency changes can arrive without a session edit, while old and new plans have different delay buffers/history. Specify which event triggers a rebuild, how audio keeps running while it builds, when a new plan becomes active, and whether a crossfade/silence/flush is allowed. Without this, the immutable-plan boundary is insufficient for sample-correct runtime PDC.

## Minor / clarity, but worth fixing in the frozen text

- `core-json.md:347–355` requires `schema.unknown_member` hints to contain the closest known member; :358–360 defines a closest-match threshold that can return none. State the fallback hint for names with no near match. `schema.type` likewise requires an example from `.example()` or a default, but both are optional (:242–250, :345–349); specify a generated example or a type-only fallback.
- `core-json.md:145–148` makes `writeCanonical` return a plain string while `Value(double)` accepts nonfinite values (:57) and `write` returns `Result` for them. State that canonical writes require prior validation or return `Result`; avoid an unchecked file-boundary abort.
- `notation.md:213–216` lets layered notes extend past a bar end, while :49 requires `Sequence::length >= end of last event`. Say whether the compiler extends item length, rejects an explicit shorter `length`, or trims the note. Do not let a bar-sum check mask this.
- `notation.md:224–226` documents `voice keys ... octave 4`, but the voice grammar at :135–139 has no `octave` option. Add it, including its allowed range and behavior with slash chords.
- `notation.md:321–325` does not specify integer rounding for ramps or how `ppq / 16` behaves when PPQ is not divisible by 16; different implementations will produce different CC sequences. Define rounding/tie policy, endpoint inclusion and duplicate-point behavior before promising deterministic decompile collapse.
- `notation.md:193–195` uses `t` both as the thirty-second duration token and exact-tick suffix (`700t`); grammar is distinguishable by prefix but especially error-prone for LLMs. A clearer exact syntax such as `ticks(700)` would reduce mistakes. Also specify whether `r:*` at an already complete bar is a no-op or invalid zero-duration rest.
- `notation.md:413–445` does not define canonical tie/chord ordering, copied-bar selection tie-breaks, or symbol spelling when multiple shortest duration tokens exist. “Deterministic” needs a total ordering, even if round-trip is correct.

## Five notation trials using only the frozen spec

These are hand-written candidate documents, not compiler executions (the implementation is in flight). Counts use 4/4, PPQ 960, one quarter beat = 960 ticks. A written bar must total 4 beats; controller lanes have no bar sum.

### 1. Syncopated funk bass with ties

```
notation 1
meter 4/4
voice bass ch 1 vel 100
1: C2:s r:s C2:s~ C2:s r:e Eb2:s r:s G1:s r:s Bb1:s r:s r:e C2:e~
2: C2:e r:e C2:s r:s Eb2:e G1:q r:q
```

Bar 1: four sixteenths 1 beat; `r:e` 0.5; `Eb2:s r:s` 0.5; `G1:s r:s Bb1:s r:s` 1; `r:e C2:e` 1 = **4 beats** (16 sixteenth steps). Bar 2: `e+e+s+s+e+q+q` = `0.5+0.5+0.25+0.25+0.5+1+1` = **4 beats**. The first tie joins two C2 sixteenths inside bar 1; the last C2 eighth ties to the first event of bar 2. I had to infer that a `~` attached to an event at the bar end searches the next numbered bar rather than only the same line. The text says “next event in the same voice,” so this inference is reasonable but deserves a test.

### 2. Triplet-feel shuffle drums

```
notation 1
meter 4/4
voice drums ch 10 grid e3 len e3 vel 96 accent 120 ghost 44
1: kick  x.....x.....
1: snare ...x.....x..
1: hat   x.xx.xx.xx.x
```

Each pattern has **12 characters**, each `e3` = 1/3 beat, so each row covers **4 beats**. The hat uses `x.x` on each triplet eighth group (first and third subdivision), the intended long-short shuffle. I had to count characters manually; spaces are optional/ignored, but there is no grouped triplet display in canonical form. The spec does not say whether `len e3` is a gate length for closed hats or whether overlapping hits on the same instrument/step can be represented. The latter should force non-grid decompilation.

### 3. Jazz ii–V–I comp with chord symbols

```
notation 1
meter 4/4
voice keys ch 2 vel 72
1: {Dm7}:h {G7}:h
2: {Cmaj7}:w
```

Bar 1: half + half = **4 beats**; bar 2: whole = **4 beats**. This depends on the default chord octave 3. The text describes an `octave 4` override but omits it from the voice grammar; a writer cannot reliably choose register. “Close position, root position” plus 9ths/11ths/13ths also needs a precise octave/ordering rule to predict pitches and keep the promise of deterministic voicing.

### 4. Melody with a pickup and a note held across three bars

```
notation 1
meter 4/4
voice melody ch 1 vel 88
1: r:h r:q G4:q~
2: G4:w~
3: G4:q r:h r:q
```

Bar 1: 2+1+1 = **4 beats**; bar 2: 4; bar 3: 1+2+1 = **4 beats**. The tie produces one G4 note spanning the tail of bar 1, all of bar 2 and the first beat of bar 3 (six beats total). This is only a **pickup-like phrase within a full first bar**, not an actual one-beat anacrusis or a MIDI item beginning one beat before the project downbeat. The grammar forbids a short first bar and `CompileContext` has no item start offset, so the requested musical pickup cannot be placed unambiguously on the project timeline.

### 5. Filter-sweep CC lane

```
notation 1
meter 4/4
lane cc cutoff ch 1
1.1 20
2.1 110 lin
3.1 110 step
4.1 35 slow
```

Lanes have **no bar-sum rule**. Positions are bar 1 beat 1, bar 2 beat 1, etc.; `cutoff` maps to CC74. The `lin` at 2.1 means the arriving segment from 1.1 ramps 20→110; `step` at 3.1 holds then jumps to 110 (no value change); `slow` at 4.1 eases 110→35. I had to infer integer rounding of the generated CC values and whether `step` with the same value emits an event. The lane is MIDI CC 0..127, not a physical-Hz stock-plugin filter automation lane; an LLM may confuse `cutoff` with the semantic `filter.cutoff` parameter from ADR-0010.

## What the product prompt already covers well, and what remains unowned

The ADR set addresses the central prompt requirements: one typed command bus/MCP (0007), branches/undo including plugin state (0008), deterministic text documents (0009), semantic stock-plugin parameters (0010), audio backends (0004), X11 editor embedding (0005), Lua (0006), and template use (0011). The location/capitalization deviation is explicitly recorded in ADR-0001, not a hidden conflict. The prompt's 64-frame benchmark, REAPER workflow, analyzer, render matrix and human review queue appear in architecture/STATUS or future-phase tasks; their detailed specs do not yet exist, which is appropriate if explicitly scheduled. The main missing *cross-cutting* contracts are reliable state-event delivery/recovery, bounded plugin isolation behavior, memory/undo retention and the nonblocking model/UI handoff above. These should be settled before implementing their Phase 4/5 consumers.
