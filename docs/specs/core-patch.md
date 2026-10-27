# Spec: core-patch — patch and preset documents

**Status:** Implemented. Interface changes go through an ADR.
**Library:** `libs/core-patch` (`supple::patch`). Depends on core-base,
core-json, core-schema, core-params. Main-thread only (never on the audio
thread). Format rules: ADR-0009.

A patch is a plugin's complete, human-readable sound/setting state. It is the
primary interface for agents: they read it, write it, merge small changes
into it, diff it, and save it as a preset. It is also the plugin's CLAP state
(ADR-0011).

## 1. The document envelope

Every patch document is a JSON object whose members, in canonical order, are:

| member | type | required | meaning |
|---|---|---|---|
| `schema` | string | yes | `"supple.<plugin>-patch/<major>.<minor>"`, e.g. `"supple.eq-patch/1.0"` |
| `name` | string (≤ 128) | no | preset name |
| `author` | string | no | |
| `tags` | array of strings | no | free tags ("pad", "warm") |
| `notes` | string | no | free text (JSON has no comments; this is where they go) |
| `seed` | integer 0..2^32-1 | only for plugins with randomness | all randomness derives from it |
| *plugin body* | … | per plugin | e.g. `params` for flat plugins; `voice`, `modules`, `routes`, `macros`, `fx` for the synth |

The plugin body is described by a core-schema `Schema<Body>`; the envelope
is common and handled by core-patch.

## 2. Patch types

```cpp
namespace supple::patch {

struct Envelope {
    std::optional<std::string> name, author, notes;
    std::vector<std::string> tags;
    std::optional<std::uint32_t> seed;
};

template <class Body>
struct Patch { Envelope meta; Body body; };

using Migration = std::function<Result<json::Value>(json::Value)>;  // whole document vN.x -> vN+1.0

template <class Body>
class PatchType {
  public:
    PatchType(std::string_view plugin,       // "eq" -> schema "supple.eq-patch"
              int major, int minor,
              schema::Schema<Body> body,
              bool hasSeed,
              std::vector<Migration> migrations = {});   // migrations[i]: major i+1 -> i+2

    Result<Patch<Body>> parse(std::string_view text, std::vector<Warning>* = nullptr) const;
    Result<Patch<Body>> fromJson(const json::Value&, std::vector<Warning>* = nullptr) const;
    json::Value toJson(const Patch<Body>&) const;              // canonical member order
    std::string serialize(const Patch<Body>&) const;           // json::writeCanonical(toJson(p))

    // RFC 7396 merge of `mergePatch` into `current`, then full validation of
    // the result. Transactional: on error, nothing changes and the errors'
    // paths refer to the merged document. `schema` may be omitted from the
    // merge patch; if present it must match.
    Result<Patch<Body>> merge(const Patch<Body>& current, const json::Value& mergePatch) const;

    // Minimal merge patch turning a into b (createMergePatch on the JSON forms).
    json::Value diff(const Patch<Body>& a, const Patch<Body>& b) const;

    json::Value jsonSchema() const;        // full document JSON Schema (envelope + body)
    std::string_view schemaId() const;     // "supple.eq-patch/1.0"
};
}
```

`PatchType` is built on `schema::DocumentType` (core-json spec B.5), which
implements the version policy; core-patch adds the envelope, merge, diff,
flat patches, state framing and descriptions. *(Amended 2026-09-26.)*

**Version handling** (ADR-0009): `schema` must be present and name this
plugin's patch type (`patch.wrong_type` otherwise, naming the expected type).
Same major and minor ≤ current: strict (unknown members are errors). Same
major, newer minor: unknown members ignored with warnings. Older major:
migrations applied in order, then parsed at the current version. Newer
major: `patch.too_new`. Malformed schema string: `patch.bad_schema_id` with
the expected form. The major version must be positive and the minor
version nonnegative.

## 3. Flat-parameter patches

Most effects have no structure beyond their parameters. core-patch provides a
ready body for them, driven by a core-params table:

```cpp
struct FlatBody { std::vector<double> values; };   // physical values, indexed like the Table
class FlatPatchType {
  public:
    FlatPatchType(std::string_view plugin, int major, int minor, params::Table table, bool hasSeed);
    // Same API as PatchType<FlatBody>: parse, fromJson, toJson, serialize, merge, diff, jsonSchema, schemaId.
    Patch<FlatBody> defaults() const;
    enum class Detail { Full, Changed };
    json::Value toJson(const Patch<FlatBody>&, Detail = Detail::Full) const;  // Changed: only non-default params
};
```

JSON form: `"params": { "<key>": <value>, ... }` with keys in table order,
values via `params::toJson` (physical units, enum keys, bools). Readonly
params are never written. On read: missing keys take defaults (so a
`Changed`-detail document is a valid full patch); unknown keys are errors
with the closest key; values go through `params::fromJson` (unit strings
accepted), out of range is an error (no clamping).

Example:

```json
{
  "schema": "supple.eq-patch/1.0",
  "name": "Vocal cleanup",
  "params": {
    "band1.type": "high_pass",
    "band1.freq": 90.0,
    "band2.freq": 250.0,
    "band2.gain": -3.0,
    "band2.q": 1.2
  }
}
```

## 4. Plugin state framing

`std::vector<std::uint8_t> encodeState(std::string_view canonicalJson, std::uint32_t magic)`
and `Result<std::string> decodeState(std::span<const std::uint8_t>, std::uint32_t magic)`:
the CLAP state stream is `magic` (4 bytes LE), format version `2` (4 bytes
LE; version 1 is the template's binary id/value form, which a plugin may
still migrate), byte length (4 bytes LE, ≤ 16 MiB), then UTF-8 canonical
patch JSON. Truncated, oversized or non-UTF-8 input is an error; nothing is
applied until the whole document has parsed and validated.

## 5. Descriptions

Each plugin supplies `std::string describe(const Patch<Body>&)` — compact
prose an LLM can read in one glance ("Two saws detuned 7 ct → 24 dB
low-pass at 1.2 kHz, env amount +2 oct, 350 ms release"). core-patch
provides helpers so descriptions are consistent:

```cpp
std::string describeFlat(const FlatPatchType&, const Patch<FlatBody>&, Detail = Detail::Changed);
// "EQ: band1 high_pass 90 Hz; band2 bell 250 Hz -3.0 dB Q 1.20" — groups, changed params only, table order.
std::string quantity(double v, units::Unit u, int digits = 3);   // units::formatQuantity re-export
```

## 6. Presets

A preset file is a patch document saved with extension `.json` under
`<data dir>/<slug>/presets/<plugin>/` (factory presets ship read-only beside
the plugin). The file name is the slugified `name`. Nothing else is needed:
presets and patches are the same format.

DSP scripts (ADR-0032) may embed read-only factory patches in their one
source file through `factoryPatches(Patches&)`. The semantic-params
extension lists them as canonical patch documents with this envelope and
the plugin's body. They need no writable user preset file or sidecar;
player and source exports carry the declarations with the script. Native
host preset discovery and the template's preset bar expose the same values
through the framework's existing preset/state mechanism.

## 7. Tests required

A fixture plugin with a structured body (nested objects, an ID-keyed map of
variant modules, an array) and a flat fixture: parse/serialize round trips
and byte-idempotence; every version-handling path including a two-step
migration; merge success, merge failure leaving the patch unchanged with
correct paths; `diff` then `merge` reproduces the target; Changed-detail
output and re-read; state framing round trip and every framing error;
`describeFlat` golden strings.

## Implementation notes (2026-09-26)

- `PatchType` and `FlatPatchType` share a non-template `Codec` that owns the
  document ID, the ADR-0009 version policy, migrations and the envelope. It
  reuses core-schema's `DocumentId`/`parseDocumentId`/`formatDocumentId`
  rather than wrapping `DocumentType`, because the envelope members must be
  present during migrations and removed before the body is decoded; error
  codes are `patch.*` (`missing_schema`, `bad_schema_id`, `wrong_type`,
  `too_new`, `migration_failed`, `not_object`, `merge_not_object`).
- For plugins with randomness a missing `seed` means 0, and `seed` is always
  written, so documents with and without an explicit `"seed": 0` are one patch.
- `FlatPatchType` errors carry paths `/params/<key>` (JSON Pointer escaped)
  and codes from core-params (`params.unknown_key`, `params.out_of_range`,
  `params.enum`, `params.type`) plus `params.readonly` for meter keys.
- `describeFlat` groups by the first key segment in table order:
  `band1: Freq 90.0 Hz; band2: Freq 250 Hz, Gain -3.0 dB; Mix 80.0 %`, and
  returns `default settings` when nothing differs.
