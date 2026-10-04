# ADR-0009: Text formats — canonical JSON, versioning, merge patch

**Status:** Accepted, 2026-09-26.

## Context

Principle 7: project files, patches, presets, MIDI notation and analysis
reports are human-readable text, diffable, deterministic in ordering, and
versioned with a schema field. Patches need partial updates (merge-patch
semantics), validation with actionable errors, and a canonical serialization.
LLMs write strict JSON far more reliably than any bespoke syntax.

## Decision

### Structured documents are JSON

Patches, presets, projects, analysis reports, benchmark results and registry
exports are strict RFC 8259 JSON in UTF-8. (The MIDI notation is a dedicated
line-oriented text format with its own canonical form: `docs/specs/notation.md`.)

### Version field

Every document's first member is `"schema": "<ns>.<type>/<major>.<minor>"`,
e.g. `"supple.synth-patch/1.0"`, `"supple.project/1.0"`,
`"supple.analysis-report/1.0"`. `ns` is `supple` (vendor, not product name).

- Same major, same or older minor: strict. Unknown members are errors with a
  "did you mean" hint — this catches LLM typos (`"cuttoff"`).
- Same major, newer minor: accepted; unknown members are ignored with
  warnings (forward compatibility within a major).
- Project-container exception (2026-10-04): known fields and containers remain
  type-strict for every minor; unknown fields inside known objects are dropped
  with path warnings even for the current minor. Unknown top-level sections and
  the top-level `extensions` object survive load/save verbatim. A newer minor
  also emits a version warning. Project `ui` is ignored with a sidecar warning;
  unsupported `groups` is preserved with a warning (session-model §5). Other
  document types retain the general strict-minor policy above.
- Older major: migrated by a chain of pure `vN -> vN+1` functions over JSON,
  each with fixture tests. Newer major: refused with a clear error.
- Writers always emit the current version.

### Canonical serialization (byte-identical for identical content)

- 2-space indentation, `\n` line endings, trailing newline, no trailing
  commas, `": "` after keys.
- Member order: schema declaration order for schema-bound objects; for
  free-form objects, keys sorted by UTF-8 byte order.
- Numbers: integers (parsed from literals without fraction/exponent that fit
  int64) print as integers; doubles print as the shortest string that
  round-trips (`std::to_chars`), always containing `.` or `e` so the kind
  survives a round trip; `-0` prints as `0.0`; NaN/Inf are rejected at
  validation.
- Strings: raw UTF-8 (validated); escape only `"`, `\`, and control
  characters (`\n`, `\t`, `\r`, `\b`, `\f`, else `\u00XX`).
- Arrays whose elements are all scalars print on one line when the line fits
  in 100 columns, otherwise one element per line; arrays containing objects
  or arrays print one element per line.
- Parsing then writing any valid document is idempotent (tested, fuzzed).

### Collections are keyed by stable IDs

Collections whose members are edited individually are JSON objects keyed by
a stable ID or name (`"modules": {"osc1": {...}, "filt1": {...}}`,
`"routes": {"lfo1>filt1.cutoff": {...}}`), not arrays. Order, where it
matters, is an explicit member (`"order": ["osc1", "filt1"]` or an
`"index"`). This makes RFC 7396 merge patches address single members, keeps
diffs local, and makes three-way reasoning sane.

### Partial updates

Partial updates use **JSON Merge Patch (RFC 7396)**: objects merge
recursively, `null` deletes, anything else replaces (arrays replace whole).
A merged document is re-validated as a whole before it takes effect; on error
nothing changes.

### Errors

Parse errors carry line/column and a one-line excerpt with a caret; schema
errors carry a JSON Pointer path, the expected type/range/unit, and a hint.
All errors are `supple::Error` and serialize to JSON for the bus.

## Consequences

Every document diffs cleanly in git and in branch reviews. Agents can send
small merge patches instead of whole documents. Our JSON library (core-json)
must implement the canonical writer and precise error locations itself.

## Alternatives considered

JSON5/JSONC (comments, trailing commas: nicer by hand, but LLMs and tools
drift between dialects; comments can live in a `"notes"` member); TOML/YAML
(ambiguous typing, indentation errors, poor for deep trees); a REAPER-style
bespoke chunk format (no schema tooling); JSON Patch RFC 6902 (index-based
array ops are fragile for LLMs; merge patch over ID-keyed objects is simpler).
