# ADR-0017: VST3 hosting

**Status:** Accepted, 2026-09-27. Build prompt Phase 4: "Plugin hosting: CLAP
first (including the custom semantic extension), VST3 second". Builds on
ADR-0013 (the `host::Instance` API with more than one backing).

## Context

Everything in the engine, the sandbox and the optimizer talks to plugins
through `host::Instance` (clap-host). Its API is CLAP-shaped but not
CLAP-bound at the call sites: plain parameter values with min/max/default,
text conversion, state blobs, latency/tail, start/stop processing on the
audio thread, `process()` with sorted note/param/MIDI events and a
transport. ADR-0013 already added a second backing (the bridge proxy)
behind the same class.

The VST3 SDK is MIT-licensed from 3.8.0 (ADR-0003, "re-verify at use").

## Decision

1. **Vendor only what a host needs**: `pluginterfaces/` and the hosting
   helpers of `public.sdk/source/vst/hosting` (module loading, plug
   provider, parameter changes, event list, process data) plus their
   `base/` dependencies, pinned to a 3.8.x tag, under
   `third_party/vst3sdk-<tag>`. The license is re-read at vendoring time;
   anything not MIT stops the work (HUMAN_REVIEW). No VSTGUI, no samples.
2. **A third backing of `host::Instance`**, selected by
   `PluginInfo::format == "vst3"` (a new field; CLAP is the default). The
   class keeps its API; each method forwards to the VST3 backing when set,
   as it does for the bridge. `raw()` stays CLAP-only (null for VST3).
3. **Scanning**: `.vst3` bundles (`<name>.vst3/Contents/x86_64-linux/
   <name>.so`) in `$VST3_PATH`, `~/.vst3`, `/usr/lib/vst3`,
   `/usr/local/lib/vst3`; `moduleinfo.json` when present, else the factory.
   Every "Audio Module Class" becomes a `PluginInfo`: `id` =
   `"vst3:" + 32 hex digits of the class ID` (stable across versions, as
   REAPER keys VST3s), name/vendor/version from the factory, `instrument`
   from the "Instrument" subcategory. The model's `PluginRef.format` is
   `vst3` for these.
4. **Mapping.**
   - Buses: the main audio input/output buses at stereo (arrangement
     negotiated with `setBusArrangements`; mono or other layouts fall back
     like CLAP's, reported by `audioInputChannels()`), the first event
     input bus for notes.
   - Parameters: `ParamInfo` in plain values (`normalizedParamToPlain` of 0,
     1 and the default; `stepCount` → stepped/enum), text via
     `getParamStringByValue`/`getParamValueByString`; `setParam(id, plain)`
     queues a normalized change in the next block's `IParameterChanges` at
     offset 0; parameter automation from the engine arrives as param events
     with sample offsets and becomes points of the same queue. Read-only,
     hidden and bypass flags map to ours.
   - Notes: CLAP-style note events become `NoteOnEvent`/`NoteOffEvent`
     (note ids kept); raw MIDI CC/pitch bend/aftertouch map through
     `IMidiMapping` to parameters (a VST3 has no raw MIDI input), unmapped
     ones are dropped.
   - Transport → `ProcessContext` (tempo, time signature, project time in
     samples and quarters, bar position, playing/looping flags and loop
     range from ADR-0015).
   - State: one blob = a small container (magic, version) holding the
     component state and the controller state, restored in that order with
     `setComponentState` in between (the SDK's documented sequence).
   - Latency: `getLatencySamples()`, re-read after `restartComponent
     (kLatencyChanged)`; tail: `getTailSamples()` (`kInfiniteTail` →
     UINT32_MAX).
   - Threading: component/controller creation, `setActive`, state and
     parameter queries on the main thread; `setProcessing(true/false)` and
     `process()` on the audio thread (our start/stop processing), which
     matches the SDK's guidance; `IComponentHandler` edits (plugin GUIs,
     later) are queued to the main thread like CLAP output events.
5. **No semantic view**: VST3 plugins have no semantic extension
   (`info().semantic` false). The LLM-facing commands work through plain
   parameters and their text conversion, exactly as for non-semantic CLAPs.
6. **Isolation works unchanged**: the bridge hosts VST3 instances with the
   same backing inside (`<slug>-bridge` links it); the protocol carries the
   format.
7. **Tests** use a small MIT VST3 test plugin built in-tree from the SDK's
   headers (gain with one parameter, an instrument that emits a sine per
   note, a latency parameter), not a third-party binary: scan, params and
   text, state round trip, bit-exact processing vs a reference, notes,
   latency change, isolation, and the engine's RT ≡ offline test with a
   VST3 in the chain.

## Consequences

- One engine code path for both formats; the model already names the
  format. Plugin lists show both; `fx.add {plugin}` accepts a VST3 id.
- VST3 editors (GUI embedding) wait for Phase 5 with CLAP GUIs.

## Alternatives considered

Hosting VST3 through a VST3→CLAP adapter (no permissive, maintained one
exists for hosting direction); a separate `Instance` subclass hierarchy
(every call site would have to know the format; the forwarding backing is
already the pattern); vendoring the whole SDK (tens of MB of samples and
VSTGUI we do not need).

## Implementation notes (2026-09-28)

- The SDK subset is `third_party/vst3sdk-v3.8.0_build_66` (`9fad977`, MIT,
  every file checked); `tp-vst3-host` builds the module loader, event list,
  host classes and string helpers. `host/clap-host/VST3.md` has details.
- Parameter changes use preallocated bounded queues (`FixedChanges`), not the
  SDK's `ParameterChanges` (its point vectors grow on the audio thread).
  rtsan is clean for the in-tree module; a plug-in's own `process` and value
  conversions are its responsibility.
- Modules are never unloaded (a strong, deliberately leaked cache): two
  locally built plug-ins exported the same GNU-unique `fmt` symbols, and
  unloading one after the scan crashed the process at exit.
- Host sequence as the SDK's: a host context (`IHostApplication` with our
  name and `IMessage` creation) for `initialize`; a separate controller is
  connected to its component (`IConnectionPoint`) and gets the component
  state; every audio bus gets an arrangement and buffers (main buses carry
  the track, auxiliary ones silence or scratch), since strict plug-ins
  refuse a partial layout and output silence; `setProcessing` answering
  `kNotImplemented` counts as started.
- Non-semantic plug-ins (VST3 and plain CLAP): `fx.describe {full}` lists
  their plain parameters (id, name, range, default, saved value; `filter`,
  at most 512 with `params_total`); `plugin.list` reports the format.
- Checked against the locally installed VST3s (thirteen classes including
  sfizz and plug-ins with sidechain buses and eight outputs): all scan,
  instantiate, list parameters and render audio through the engine.
- Not yet: a persisted scan cache (each start loads every bundle without a
  `moduleinfo.json`), out-of-process scanning, editors (Phase 5), a
  VST3-specific crashing fixture (isolation is shared with CLAP).
