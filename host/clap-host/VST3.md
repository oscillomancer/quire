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

VST3 parameters cross the host API in plain values. The backing normalizes
them for VST3 process queues. Input and output event/parameter arenas have
fixed capacities and are allocated before processing. The SDK's
`ParameterChanges` helper is deliberately not used on the audio thread: its
point vectors grow during `addPoint`. `EventList` uses storage allocated at
construction. Controller conversions run on the main thread; process queues
and output edits carry normalized values. The engine converts automation before
processing; main-thread consumers must convert output edits before using plain values.
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
