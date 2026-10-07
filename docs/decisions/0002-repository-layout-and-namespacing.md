# ADR-0002: Repository layout and namespacing

**Status:** Accepted, 2026-09-26.

## Context

The build prompt suggests a layout (Section 5) and asks for changes to be
recorded. Several foundations it does not list are needed by everything else:
structured errors, JSON, schema binding, the command registry, MIDI data and
audio files.

## Decision

```
quire/
  README.md AGENTS.md ARCHITECTURE.md STATUS.md THIRD_PARTY.md
  cmake/            Product.cmake (the name), Defaults.cmake (target helpers)
  docs/             research/ decisions/ specs/ benchmarks/ demos/
  libs/
    core-base/      Error, Result<T>, SUPPLE_RT, RT primitives (SPSC ring, atomic swap)
    core-json/      JSON value, parser with line/col errors, canonical writer,
                    JSON Pointer, RFC 7396 merge patch
    core-schema/    typed schema DSL: bind JSON <-> C++ structs, validate with
                    actionable errors, export JSON Schema
    core-params/    semantic parameter schema (units, ranges, curves, tags)
    core-patch/     patch/preset documents: validation, merge, canonical form
    core-analysis/  offline perception layer (loudness, spectral, harmonic, ...)
    core-dsp/       shared DSP building blocks
    audio-file/     WAV/FLAC/MP3 I/O over dr_libs, sample-rate metadata
    midi/           MIDI event model, SMF read/write
    notation/       the MIDI text notation compiler/decompiler
    command-bus/    command registry, descriptors, dispatcher, doc/schema export
    control-protocol/ JSON-RPC 2.0 framing, Unix-socket server/client, events
  host/
    clap-host/      CLAP hosting library (load, instantiate, process, params,
                    state, semantic-params extension client)
    sandbox/        Phase 2 headless plugin sandbox: library + CLI
    engine/         Phase 4 headless DAW engine
    isolation/      out-of-process plugin bridge
  plugins/          template-derived stock plugins (ADR-0011) + shared
                    semantic-params extension header
  daw/              gui/, agent-panel/, lua/
  mcp/              MCP adapter (stdio, streamable HTTP), generated schemas
  skill/            generated/ (from registry + plugin schemas), guides/
  tools/            license-check/, ci/, eval-harness/, benchmark/, doc-gen/
  tests/            integration/, fuzz/, rt-safety/, golden/
  third_party/      vendored dependencies + manifest.json
```

- **Namespaces:** all C++ is in `supple::<component>` (`supple::json`,
  `supple::params`, `supple::host`). The product name never appears in code
  (ADR-0001).
- **Library shape:** `libs/<name>/include/supple/<short>/*.h` public headers,
  `src/` implementation, `tests/` doctest suites registered as
  `<name>.tests`. CMake target `<name>`, alias `supple::<name>`, created with
  `supple_add_library()` so warnings and sanitizers apply uniformly.
- **Dependency direction:** libs never depend on host/daw/plugins; host never
  depends on daw. Plugins depend only on header-light libs they need
  (core-params, core-patch, core-json, core-dsp) and their template stack.
- **Generated artifacts** (JSON schemas, skill reference, Lua stubs) are
  committed, and a test regenerates them and fails on any diff.

## Consequences

More, smaller libraries than suggested; each is testable in isolation and can
be delegated with a frozen header. The `host/clap-host` library is shared by
the sandbox (Phase 2) and the engine (Phase 4), which is how the sandbox
"becomes the seed of the DAW engine".

## Alternatives considered

A single `core` library (simpler CMake, but blurs interfaces that parallel
workers need frozen); product-named namespaces (makes renaming expensive).
