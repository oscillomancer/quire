# Architecture decision records

One numbered file per significant decision. An ADR is immutable once
**Accepted**; change a decision by writing a new ADR that supersedes it (and
mark the old one **Superseded by ADR-NNNN**). Frozen interfaces (library APIs,
command schemas, the control protocol, file formats) change only via an ADR.

Format: Status · Context · Decision · Consequences · Alternatives considered.

| # | Title | Status |
|---|---|---|
| [0001](0001-project-name-and-location.md) | Project name and repository location | Accepted (Nate may veto the name) |
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
