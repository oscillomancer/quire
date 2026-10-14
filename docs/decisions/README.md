# Architecture decision records

One numbered file per significant decision. Once an ADR is **Accepted**,
its decision changes only through a new ADR that amends or supersedes it;
the old one gets a dated note pointing to the new one (or is marked
**Superseded by ADR-NNNN**). Dated *Implementation*, *Clarifications* or
*Amendments* sections at the end record how a decision was carried out.
Frozen interfaces (library APIs, command schemas, the control protocol,
file formats, the bridge protocol) change only via an ADR.

Format: Status · Context · Decision · Consequences · Alternatives considered.

The earlier records refer to the phases of the original build plan and to
its requirements document (the "build prompt"): Phase 0 foundations, 1 core
libraries, 2 plugin sandbox and MCP, 3 stock plugins, 4 headless engine,
5 GUI, 6 skill and evaluation, 7 scripting ergonomics and a DSP language,
8 hardening and ports. Phases 0 to 5 are complete; `STATUS.md` has the
current state.

| # | Title | Status |
|---|---|---|
| [0001](0001-project-name-and-location.md) | Project name and where it is defined | Accepted |
| [0002](0002-repository-layout-and-namespacing.md) | Repository layout and namespacing | Accepted |
| [0003](0003-toolchain-and-dependencies.md) | Toolchain, dependency vendoring and license gate | Accepted |
| [0004](0004-audio-backend.md) | Audio backend strategy | Accepted |
| [0005](0005-gui-and-windowing.md) | GUI toolkit and windowing (XWayland) | Accepted |
| [0006](0006-embedded-scripting.md) | Embedded scripting language | Accepted |
| [0007](0007-command-bus-and-control-protocol.md) | Command bus, registry, control protocol and MCP | Accepted |
| [0008](0008-session-model-undo-branches.md) | Session model, stable IDs, undo and branches | Accepted |
| [0009](0009-text-formats.md) | Text formats: canonical JSON, versioning, merge patch | Accepted |
| [0010](0010-semantic-parameters.md) | Semantic parameters and value domains | Accepted |
| [0011](0011-stock-plugins-from-template.md) | Stock plugins built from the template | Accepted |
| [0012](0012-anticipative-processing.md) | Anticipative processing and the worker pool | Accepted |
| [0013](0013-plugin-isolation.md) | Plugin process isolation | Accepted |
| [0014](0014-recording-and-live-nodes.md) | Recording, input monitoring and the live region | Accepted |
| [0015](0015-loop-and-punch.md) | Transport loop, punch, and loop recording into takes | Accepted |
| [0016](0016-midi-input.md) | MIDI input — monitoring and recording | Accepted |
| [0017](0017-vst3-hosting.md) | VST3 hosting | Accepted |
| [0018](0018-sidechain-inputs.md) | Sidechain inputs | Accepted |
| [0019](0019-console9-strips.md) | Typed strips with Console 9 summing | Accepted |
| [0020](0020-auxiliary-channels.md) | Auxiliary channels and plug-in pins | Accepted |
| [0021](0021-multi-out-instruments.md) | Multi-output plug-ins | Accepted |
| [0022](0022-strip-polarity-and-mono.md) | Strip polarity and mono | Accepted |
| [0023](0023-background-worker-participation.md) | Background worker participation and parking | Accepted |
| [0024](0024-live-multiprocessing.md) | Live callback multiprocessing | Accepted |
| [0025](0025-floating-point-environment.md) | Engine floating-point environment and first touch | Accepted |
| [0026](0026-background-thread-scheduling.md) | Background audio thread scheduling | Accepted |
| [0027](0027-midi-learn.md) | MIDI learn: controllers drive parameters and actions | Accepted |
| [0028](0028-renderer-wakeups.md) | Renderer deadlines and event-driven wakes | Accepted |
| [0029](0029-shared-main-thread-wake.md) | Shared main-thread notification relay | Accepted |
| [0030](0030-main-thread-scheduling.md) | Main-thread service notifications and deadlines | Accepted |
| [0031](0031-bridge-parent-notifications.md) | Bridge-to-parent notifications and idle audio parking | Accepted |
| [0032](0032-dsp-scripting.md) | DSP scripting: a checked C++ subset, a JIT player and exports | Accepted |
| [0033](0033-daw-and-plugin-licensing.md) | DAW and plugin licensing | Accepted |

| [0036](0036-native-plugin-hosting.md) | Native plugin hosting and supervised scanning | Accepted |
