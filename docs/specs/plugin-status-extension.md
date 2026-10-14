# Spec: CLAP extension `com.suppleaudio.plugin-status/1`

**Status:** Implemented. Header: `plugins/common/include/supple/clap/plugin_status.h`,
with a byte-identical copy under `plugins/suite/semantic/include/supple/clap/`
for source exports. Any plugin may implement it; it is independent of
semantic-params and is not part of a patch or saved project.

## 1. Interface

```c
static const char CLAP_EXT_SUPPLE_PLUGIN_STATUS[] = "com.suppleaudio.plugin-status/1";
typedef struct clap_plugin_supple_plugin_status {
    uint32_t struct_size;
    bool(CLAP_ABI *get_status)(const clap_plugin_t* plugin, const clap_ostream_t* out);
} clap_plugin_supple_plugin_status_t;
```

`get_status` is main-thread only. It writes a UTF-8 JSON document to the
stream, returns false on failure, and never consumes or clears problems.
The host requires `struct_size` through `get_status` and a non-null callback.
Future compatible members append to the struct; incompatible changes use `/2`.

## 2. Document

```json
{
  "schema": "supple.plugin-status/1.0",
  "problems": [
    {
      "code": "script.runtime.budget",
      "message": "The script exceeded its loop budget and was stopped.",
      "scope": "audio",
      "line": 14,
      "hint": "Fix the loop and save the script again, or restart the plugin.",
      "silent": true
    }
  ]
}
```

`problems` is an array, empty when no current problems are known. Every
entry requires nonempty `code`, `message`, `scope`, `hint` strings and a
boolean `silent`: the plugin is currently silenced **because of this
problem**, not merely processing silence. `line`, when known, is a positive
one-based source line; omit it otherwise. Scopes include `audio`, `editor`
and `reload`; hosts accept other scopes and ignore additional fields.
Codes belong to the implementation (`script.*` for DSP scripts).

Problems are current observations, not a log history. Queries do not run
audio or recompile code. Audio observations may change while main reads
them; a query during a block/reset can observe a transition. Plugins must
publish them with bounded lock-free mechanisms: no allocation, locking,
logging, streams or JSON on the audio thread.

## 3. Script implementation and lifecycle

The suite's `ScriptLayer` exposes:

- `script.runtime.budget`: the hook budget was exceeded, with its first
  source line and actual stopped/silent state.
- `script.runtime.index`: an index was out of range; the read returned zero
  or the write was discarded. It does not itself silence the plugin.
- `script.editor.budget` / `script.editor.index`: observations from a
  script editor that has run, including offscreen pictures, scoped to the
  editor and never marked audio-silent.
- `script.reload.failed`: a saved revision could not replace the loaded
  code. The previous version stays loaded; the first compiler error and
  its line/hint are included when available. ID changes and unreadable or
  refused replacements also report a corrective hint.

Audio observations clear when replacement code is adopted, on reset or on
activation. A faulty generation is not crossfaded into replacement code,
so its faults cannot stop the new generation. Valid code still crossfades.
Editor observations clear when replacement code rebuilds the editor.
Reload errors clear after an accepted replacement or restoration of the
last accepted source. A reload waiting for a host restart keeps old audio
observations until the replacement actually becomes active. Source exports
can report index errors without a source line; their C++ code does not have
the JIT's loop-budget instrumentation.

## 4. Host and bridge

`host::Instance::pluginStatus()` returns the validated document, null for
an unsupported extension, or a structured error on a failed/invalid report.
`host.status` and `host.status_contract` carry corrective hints.
Bridge protocol **12** appends `PluginStatus` after `AckNotifications`:
no request payload, one length-prefixed JSON string reply (`null` if
unsupported). Existing operation numbers and shared-memory layout stay
unchanged. The child calls the same API on its plugin's main thread; shared
and dedicated instances therefore report their own problems. Existing
version negotiation rejects a mismatched bridge. Bridge failure is an error
from `pluginStatus()`, never an empty healthy report.

Engine `fx.describe` and sandbox `plugin.describe` expose the document's
`problems` in both summary and full modes and `status_available: true`.
Without support, or without a live engine instance, `status_available` is
false and `problems` is omitted. Unavailable status does not imply health.
A failed query (a crashed bridge, an invalid document) does not fail the
description: `status_available` is false and `problems` holds one entry with
scope `host`, the error's code (`host.status`, `host.status_contract`, a
bridge code) and its hint (`host::statusFailure`).
Engine status always comes from the live instance, never a scratch probe.
