# Spec: CLAP extension `com.suppleaudio.semantic-params/1`

**Status:** Frozen for Phase 2 implementation (2026-09-26). The extension ID
carries the major version; incompatible changes get `/2`. Additions within
`/1` only append members after a version check (see §4).
**Header:** `plugins/common/include/supple/clap/semantic_params.h` (C ABI,
usable from C and C++, depends only on the CLAP headers).
**Implementations:** plugin side in `plugins/common` (C++ helper over
core-params/core-patch); host side in `host/clap-host`.

## 1. Purpose

Standard CLAP gives hosts anonymous parameters (a name, a range, value
text). This extension lets a host that understands it — the Quire sandbox
and engine — see a stock plugin the way its designers do: parameters with
keys, physical units, curves and roles; a structured, versioned JSON patch
that can be read, replaced, merged and validated with actionable errors; and
a prose description of the current sound. Hosts that do not know the
extension use standard `clap.params` and `clap.state`, and lose nothing but
the semantics.

## 2. Plugin interface

```c
#include <clap/clap.h>

static const char CLAP_EXT_SUPPLE_SEMANTIC_PARAMS[] = "com.suppleaudio.semantic-params/1";

enum {
    CLAP_SUPPLE_PATCH_DETAIL_FULL = 0,      /* every non-readonly value */
    CLAP_SUPPLE_PATCH_DETAIL_CHANGED = 1,   /* only values differing from defaults */
};
enum {
    CLAP_SUPPLE_SET_REPLACE = 0,            /* the document is a complete patch */
    CLAP_SUPPLE_SET_MERGE = 1,              /* RFC 7396 merge patch into the current patch */
    CLAP_SUPPLE_SET_VALIDATE_ONLY = 2,      /* validate as MERGE would, change nothing */
};

typedef struct clap_plugin_supple_semantic_params {
    /* Size of this struct, for forward compatibility (see §4). */
    uint32_t struct_size;

    /* [main-thread] Writes the semantic schema document (JSON, §3.1) to `out`.
     * Returns false on stream failure. The schema may change only after the
     * plugin calls host->changed(CLAP_SUPPLE_CHANGED_SCHEMA). */
    bool(CLAP_ABI *get_schema)(const clap_plugin_t *plugin, const clap_ostream_t *out);

    /* [main-thread] Writes the current patch as canonical JSON (core-patch). */
    bool(CLAP_ABI *get_patch)(const clap_plugin_t *plugin, uint32_t detail, const clap_ostream_t *out);

    /* [main-thread] Applies `json` (UTF-8, `size` bytes, not NUL-terminated)
     * with the given mode. Transactional: either the whole document applies or
     * nothing changes. Returns true on success. On failure returns false and,
     * if `errors` is non-NULL, writes a JSON array of error objects (§3.3).
     * May be called while the plugin is active and processing (§5). */
    bool(CLAP_ABI *set_patch)(const clap_plugin_t *plugin, uint32_t mode,
                              const char *json, uint32_t size, const clap_ostream_t *errors);

    /* [main-thread] Writes a compact prose description of the current patch
     * (UTF-8 plain text, ≤ 600 characters for DETAIL_CHANGED). */
    bool(CLAP_ABI *describe)(const clap_plugin_t *plugin, uint32_t detail, const clap_ostream_t *out);
    /* [main-thread] Optional compact live supple.analysis-report/1.0 JSON.
     * NULL for plugins without telemetry; false when unavailable or the stream
     * fails. Check struct_size through this member and its pointer first. */
    bool(CLAP_ABI *get_telemetry)(const clap_plugin_t *plugin, const clap_ostream_t *out);
} clap_plugin_supple_semantic_params_t;
```

## 3. Documents

### 3.1 Schema document

```json
{
  "schema": "supple.semantic-schema/1.0",
  "plugin": { "id": "com.suppleaudio.eq", "name": "…", "version": "0.1.0" },
  "patch_type": "supple.eq-patch/1.0",
  "params": { …core-params describeTable() "params" member… },
  "patch": { …JSON Schema (2020-12) of the patch document… },
  "docs": "One paragraph: what the plugin is and how its patch is organised."
}
```

`params` is exactly the `params` member of `core-params::describeTable`;
`patch` is `PatchType::jsonSchema()`. Keys, ids and units are the same ones
the plugin exposes through `clap.params` (the CLAP values follow ADR-0010).

### 3.2 Patch documents

As specified in `docs/specs/core-patch.md`. `get_patch` output is canonical.
For flat plugins, the patch's `params` object uses the same keys as the
schema's `params`.

### 3.3 Errors

A JSON array of `supple::Error` objects:
`[{"code": "...", "message": "...", "path": "/params/band2.gain", "hint": "...", "details": [...]}]`
(`loc` is omitted; `path` is a JSON Pointer into the submitted document).
A `set_patch` failure caused by a stream write error returns false with an
empty array.

## 4. Versioning within `/1`

Hosts read `struct_size` and must not call members beyond it. Plugins set it
to `sizeof(clap_plugin_supple_semantic_params_t)` of the header they were
built with. New members are only ever appended.

The optional `get_telemetry` member was appended for the analyzer (2026-09-29).
To call it, require `struct_size >= offsetof(type, get_telemetry) +
sizeof(extension->get_telemetry)` and a non-null pointer. Original members
remain usable when `struct_size >= offsetof(type, get_telemetry)`; hosts must
not require the new full struct size just to call the old members. Existing
plugins expose a null callback. A telemetry report does not change patch state.
Gauge reports the existing analysis-report source/loudness/spectral/stereo
fields it measures, single-point current M/S series, the 31-band live spectrum,
and additive `live` window/hold/dropped-frame metadata. See stock-effects §9.

## 5. Threading, real-time safety, parameter sync

- Every function is main-thread only.
- `set_patch` on an active plugin must not block or allocate on the audio
  thread. The plugin validates and builds the new state on the main thread,
  then hands it to the audio thread with a wait-free mechanism (atomic
  values plus a generation counter, or an atomic pointer swap with deferred
  reclamation on the main thread). The change takes effect at the start of a
  process block; the parameter smoothers treat it like host automation
  (smoothed), except that a `set_patch` issued while the plugin is not
  processing snaps values (like state load).
- After a successful `set_patch` that changes parameter values, the plugin
  calls `clap_host_params.rescan(host, CLAP_PARAM_RESCAN_VALUES)` so generic
  views update, and calls the host extension's `changed(PATCH)` (§6). It does
  not emit param-value output events for these changes (they are state
  changes, not gestures).
- `get_patch` reflects every change applied so far, including host
  automation and GUI edits that have reached the main thread.
- The CLAP state (`clap.state`) is the canonical patch JSON framed as in
  core-patch §4; `state.load` applies it with REPLACE semantics.

## 6. Host interface (optional for plugins to use)

```c
enum {
    CLAP_SUPPLE_CHANGED_PATCH = 1u << 0,   /* patch values changed (GUI edit, preset load in plugin UI) */
    CLAP_SUPPLE_CHANGED_SCHEMA = 1u << 1,  /* schema changed (rare: e.g. module slot types changed availability) */
};
typedef struct clap_host_supple_semantic_params {
    uint32_t struct_size;
    /* [main-thread] Notifies the host that it should re-read patch and/or schema. */
    void(CLAP_ABI *changed)(const clap_host_t *host, uint32_t flags);
} clap_host_supple_semantic_params_t;
```

Plugins call `changed(PATCH)` when their own GUI changes structure or loads
a preset (in addition to the standard `clap_host_state.mark_dirty`), so the
host can capture an undo step (ADR-0008).

## 7. Conformance tests (both sides)

A test host in `host/clap-host/tests` loads each stock plugin and checks:
schema parses and its `params` match `clap.params` (ids, names, CLAP ranges
derived per ADR-0010); `get_patch` → `set_patch(REPLACE)` round trip is
identity; `CHANGED` detail re-applies to the same state; merge of a single
value changes only that value (compared through `get_patch`); invalid
documents fail with errors whose paths point at the problem and leave
`get_patch` unchanged; `VALIDATE_ONLY` never changes state; `set_patch`
during processing (another thread calling process with rtsan active) is
RT-clean and takes effect within one block; state save/load equals patch
round trip; `describe` is non-empty and ≤ 600 chars.

## Implementation notes (2026-09-26, orchestrator)

- `plugins/common` `ParamStore` delivers whole patches to the audio thread
  with a seqlock: `assign()` (set_patch, state load, reset) is one write;
  the audio thread calls `readAll(values)` when `generation()` moved and keeps
  its previous values if a write is in progress (wait-free; retried next
  block). Single CLAP param events still update one value at a time.
- Legacy state: `FlatSemantic::legacyLoad` is only offered streams that do not
  claim the framed form (`patch::isFramedState`); a malformed v2 frame is
  rejected, never reinterpreted.
- The conformance tool's step 7 runs 200 merges against a thread that
  processes blocks continuously and checks for process errors and that the
  last patch is in effect; RT cleanliness is checked when the plugin and tool
  run under the rtsan preset.

## Amendments (2026-09-26, orchestrator — structured patches)

- **Flat `params` in merge patches.** A plugin whose patch is structured
  (slots, routes — e.g. Orrery) also accepts a top-level `"params"` member in
  `CLAP_SUPPLE_SET_MERGE` / `VALIDATE_ONLY` documents: `{"params": {"<key>":
  value}}` with keys as in the schema's `params`. The plugin translates each
  key to its place in the nested document. Keys of slots the patch does not
  have are errors (`/params/<key>`). Hosts can therefore set values by key on
  any semantic plugin (the sandbox's `param.set`, the optimizer).
- **`"active": false`** on a schema `params` entry marks a parameter the
  current structure does not use (its slot is absent, or its slot's type has
  no such field). Plugins whose schema changes this way call
  `changed(PATCH | SCHEMA)` after a structural `set_patch`.
- Structured plugins implement the extension through
  `plugins/common`'s `SemanticModel` / `SemanticAdapter` (same wire formats as
  `FlatSemantic`); `semantic-conformance` exercises them with whole-document
  replaces during processing.
