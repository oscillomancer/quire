# VST3 hosting notes

ADR-0017 is the contract. The SDK subset is pinned at `v3.8.0_build_66`
(`9fad9770f2ae8542ab1a548a68c1ad1ac690abe0`) under
`third_party/vst3sdk-v3.8.0_build_66`. Its top-level and copied subdirectory
licenses are MIT. The static `supple::vst3-host` target contains the module
loader and ABI helpers; the in-tree test module also builds the SDK's small
component/controller base classes.

`host::scan` accepts `.vst3` bundles and scans `VST3_PATH`, `~/.vst3` and the
system VST3 directories by default. It reads `Contents/Resources/moduleinfo.json`
when available, then falls back to factory metadata. IDs are `vst3:` followed
by the class's 32 hexadecimal digits. `PluginInfo::format` selects the VST3
backing of `host::Instance`, and the bridge forwards that format unchanged.

VST3 parameter queries and setters use plain values; process events use
normalized values. Input and output event/parameter arenas have
fixed capacities and are allocated before processing. The SDK's
`ParameterChanges` helper is deliberately not used on the audio thread: its
point vectors grow during `addPoint`. `EventList` uses storage allocated at
construction. Controller conversions run on the main thread; process queues
and output edits carry normalized values. The engine converts automation before
processing; main-thread consumers must convert output edits before using plain values.
Output parameter events have flags 0 and note/port/channel/key addresses -1,
because VST3 parameter-change queues do not supply CLAP event flags or note
addresses. Native VST3 note-off events are not CLAP note-end notifications.
`takeIdleOutputs()` is the CLAP params.flush channel; it returns no VST3 events.
A plugin that allocates or locks inside its own `process` cannot be made RT safe
by the host.

Modules are opened once and never unloaded (see ADR-0017's notes). Plug-ins
get a host context for `initialize`; separate controllers are connected to
their components and given the component state; every audio bus is declared
and gets buffers (auxiliary buses silence or scratch space, allocated in
`activate`).

The test module exposes gain, a per-note sine instrument, latency change and
a strict sidechain gain (refuses a partial bus layout, answers setProcessing
with kNotImplemented).
Host tests cover metadata, parameter text, state, sample offset automation,
notes, latency and isolated processing. The engine test checks a VST3 effect
in a chain against the offline render. A VST3-specific crashing or hanging
fixture is not included; the bridge's process-failure paths are exercised by
the existing CLAP fixtures.

A state load is a queued-edit barrier: it advances the edit generation, and
only the audio consumer removes old queue entries. Saving an inactive
instance first sends unconsumed edits through the VST3 zero-sample processor
flush (no audio buses), then saves component and separate controller state.
`setParam` followed by inactive `saveState` therefore persists the edit;
active saves require the audio consumer to have delivered queued edits first.
The fixture includes a separate-controller gain to exercise this distinction.

Audio layouts are bounded at 128 buses per direction and 64 channels per bus;
oversized declared or negotiated layouts fail with `host.layout`. Main widths
are reported accurately; bridged main/key buses have a further 16-channel cap.
Notes are tracked with counts and note ids in a bounded arena. Wildcard note
ends and chokes expand to explicit offs; reset releases every tracked count.
Run-loop snapshots dispatch only handlers that remain registered.

Native module loaders (ADR-0036) are copied without modification from public.sdk
commit `a3911a4615dabbfdfd9d181ee26b05c70c289a95`, the submodule pinned by
SDK commit `9fad977`. macOS builds module_mac.mm with Cocoa/CoreFoundation;
Windows builds module_win32.cpp with shell32/ole32/uuid. The in-tree fixture
uses native bundle binary paths and entry points. See sandbox.md for search
paths, supervised scanning and native bridge/editor behavior on all three platforms.
