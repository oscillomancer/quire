# Gate integration

Identity: functional id `gate`, display string defined once in engine params,
CLAP id `com.suppleaudio.gate`, state `QGT1`, AU subtype `QGat`, effect `aufx`.
The template was copied and renamed before implementation; no upstream file
was modified. MIT is retained under ADR-0011, overriding the copied upstream
licensing checklist.

The SDK-free engine replaces the placeholder gain processor. It owns linked
peak detectors, two Butterworth key sections per channel, gate/expander
states, sample-counted hold, dB one-pole ramps and preallocated main/key delay
storage. Continuous controls use 20 ms ramps, switches 10 ms; coefficient
updates use a fixed 16-sample grid. The normalized lookahead opening ramp
reaches unity before the delayed onset. Latency edits are deferred until
reactivation; dry mix and key listen share the main delay. All DSP primitives
stay inside this engine; no cross-plugin linking or shared-library edits.

The wrapper uses ParamStore, ClapParams and FlatSemantic for parameters,
patches and transactional framed JSON state. Its describe thunk calls the
engine's signal-flow summary because the generic helper permits 600 characters
and effects require 300 for changed-only detail. Readonly gain reduction and
logical open state publish at most every 50 ms; the GUI receives small frames
only while visible. Latency callbacks are coalesced and the activated latency
stays stable during a pending restart.

The main bus supports mono/stereo and both precisions, with a second stereo
non-main sidechain input. The product smoke harness checks both input ports
and the equivalent main-port contracts locally: the shared ports helper
assumes exactly one effect input. It also exercises actual stop/deactivate/
reactivate for lookahead: the template smoke assumed an active latency edit
could report its new delay immediately. Shared host_contract.h is unchanged;
its parameter text and float/double/in-place render checks still run.

The editor has a linked key-level display with threshold/hysteresis band,
logical state lamp, four-second gain history and two rows of dynamics,
sidechain and output controls. All metadata comes from the table. Keyboard,
250 ms wheel gestures, drag/focus handling and renderer isolation are kept.
The preview feeds deterministic synthetic drum bursts through the real engine
for default and drums settings. Its palette and typography match Orrery.

The monorepo CMake shape follows the verified analyzer/probe-synth integration,
with an isolated engine binary directory and vendored dependency paths. The
unused template gain CLI is removed. Window, renderers, widgets, Ring, entry
points, fonts, wrapper patches, Linux probe and shared host contract are
byte-identical after identity renames; the generic probe's meter id is gr.
The format gate excludes the shared infrastructure.

Useful template proposals: sidechain-aware host-port checks, deferred latency
restart assertions and a compact engine describe seam. No template edits or
new dependencies were needed. STATUS and independent review are orchestrator
work; REAPER/platform acceptance remains separate from these device-free tests.
