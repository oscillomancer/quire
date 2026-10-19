# ADR-0011: Stock plugins built from the template

**Status:** Accepted, 2026-09-26. Evidence: `docs/research/template.md`.
(2026-10-18: amended by ADR-0047 for stock plugins written as DSP scripts,
which live in `plugins/stock/` and build from `plugins/template`.)

## Context

Stock plugins must be created from Supple Audio's plugin template the way it
is designed to be used, never by modifying it in place. They must also expose
core-params semantic parameters and core-patch text patches through a custom
CLAP extension, and build inside the Quire monorepo. The survey found that
unmodified template copies cannot coexist in one CMake build (global
`starter-*` targets and functions, `STARTER_*` options, FetchContent names,
`${CMAKE_SOURCE_DIR}`-relative paths, shared `${CMAKE_BINARY_DIR}/starter_assets`),
that it is an effect (an instrument needs note ports and note processing),
and that its parameter table is linear-only.

## Decision

1. **Instantiation.** Each plugin is a copy of the template under
   `plugins/<name>/`, renamed per the template's identity checklist
   (`Starter`→`<Name>`, `starter`→`<name>`, `STARTER`→`<NAME>`,
   `com.suppleaudio.<name>`, new AU subtype, new state magic). Infrastructure
   (wrapper, GUI stack, host_contract, probes) stays as the template ships
   it. The shared GUI files stay byte-identical to the template apart from
   the product namespace, per the template's own rule.
2. **Monorepo build.** Plugins are built as **ExternalProject**s of the
   top-level build, each configured with its own binary dir and explicit
   artifact paths, with FetchContent sources supplied from one shared,
   pinned dependency cache (`FETCHCONTENT_SOURCE_DIR_*`, `CLAP_SDK_ROOT`,
   `VST3_SDK_ROOT`), which the survey showed works offline. This keeps each
   plugin's CMake identical in shape to the template's, avoids target and
   option collisions without editing the template's CMake structure, and
   keeps "copy the template" meaning exactly that. The top-level build owns
   the pins; the license gate checks them.
3. **Semantic parameters.** The template's rule "one parameter table is the
   only metadata" is kept by making that table a core-params definition:
   `<Name>Params.h` declares core-params `ParamSpec`s (stable CLAP id, key,
   unit, range, curve, tags, …); the wrapper's `paramInfo`,
   `value_to_text`/`text_to_value` and the editor read it through
   core-params helpers. Normalization follows ADR-0010.
4. **The extension** `com.suppleaudio.semantic-params/1` (C ABI, spec in
   `docs/specs/semantic-params-extension.md`) is implemented once in a small
   shared library `plugins/common/` (links core-params, core-patch,
   core-json) and returned from `plugGetExtension`. Hosts without it use
   standard CLAP params and state.
5. **State.** CLAP state becomes the canonical core-patch JSON text (with the
   template's magic/version framing kept as a header so the loader can still
   recognise and migrate the binary form). Loading is transactional: parse
   and validate fully, then apply; on error nothing changes.
6. **Template improvements** discovered along the way (e.g. `parseValue`
   accepting trailing junk, non-transactional state load, no size cap on
   state input, an instrument variant, the semantic-params hook) are proposed upstream as template patches, not silently forked.
7. **Names and licensing.** Stock plugins get their own names (chosen per
   plugin, product-name-independent), CLAP ids `com.suppleaudio.<name>`.
   Template-derived files keep their MIT headers (Supple Audio's own code);
   Quire's own license is the maintainer's choice.

## Consequences

A plugin directory is recognisably the template, so fixes flow both ways.
Every plugin build is independent (one failing plugin does not block the
rest) at the cost of a slower full build. core-params becomes a header-light
dependency the plugins can include without pulling the host stack.

## Alternatives considered

`add_subdirectory` of renamed copies (works only after deeper edits to the
template's CMake — prefix functions, cache options, dependency ownership —
which would drift from the template); one multi-plugin CLAP binary (not how
the template works; complicates per-plugin validation and GUI identity).

## Clarifications (2026-09-26, from design review)

- **What stays identical to the template vs what the product owns.** Kept
  byte-identical apart from namespace/licence header (the template's own
  shared-code rule): `Telemetry.h` (`Ring`), `test/host_contract.h`, the GUI
  stack (`Window.*`, `Gui.h`, renderer backends, Pugl/NanoVG integration),
  the clap-wrapper patches, the probes. Product-owned, as the template
  itself expects: the parameter table, the DSP core, the `*_clap.cpp`
  sections for params/state/extensions, `Theme.h`/`Widgets.h`/`Editor.cpp`,
  CMake identity. The semantic-params hook, the JSON state and transactional
  loading are product-owned changes in our copies, each with tests; the
  generally useful ones are *also* offered upstream as template patches.
  Work does not pause on those proposals.
- **C++20.** The template sets C++17. Our copies set C++20 for their own
  targets (a product build setting); raising the template itself is offered
  as a template patch.
- **Clean-clone builds.** The template fetches clap-wrapper, Pugl and NanoVG
  (and, via CPM, the CLAP and VST3 SDKs) from the network. For the
  one-command offline build, their sources are vendored under
  `third_party/` at the template's pins and passed to each plugin's
  configure with `FETCHCONTENT_SOURCE_DIR_*` / `CLAP_SDK_ROOT` /
  `FETCHCONTENT_FULLY_DISCONNECTED=ON`. The default monorepo build produces
  **CLAP** (what Quire hosts and REAPER loads). VST3 is built when
  `SUPPLE_PLUGIN_VST3=ON` and the VST3 SDK is vendored or present (it is
  large; vendoring it is deferred until VST3 builds are needed). A clean
  `/tmp` clone + build is the gate (`tools/ci/clean-clone.sh`).
