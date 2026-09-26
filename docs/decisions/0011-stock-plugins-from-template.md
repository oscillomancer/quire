# ADR-0011: Stock plugins built from the template

**Status:** Accepted, 2026-09-26. Evidence: `docs/research/template.md`.

## Context

Stock plugins must be created from Nate's template
(`~/Projects/audio-plugins/template`) the way it is designed to be used,
never by modifying it in place (build prompt 2.3). They must also expose
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
   state input, an instrument variant, the semantic-params hook) are proposed
   to Nate in `HUMAN_REVIEW.md` with patches, not silently forked.
7. **Names and licensing.** Stock plugins get their own names (chosen per
   plugin, product-name-independent), CLAP ids `com.suppleaudio.<name>`.
   Template-derived files keep their MIT headers (Supple Audio's own code);
   Quire's license is Nate's choice (`HUMAN_REVIEW.md`).

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
