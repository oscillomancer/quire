# Command reference

Generated from the command registry.

## `analyze.compare@1`

Compare two renders or track outputs by perceptual descriptors.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `reference` | string |  |  |  |  |
| `test` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "comparison",
  "properties": {
    "notable": {},
    "ltas_rms_db": {
      "type": "number"
    },
    "summary": {
      "type": "string"
    }
  },
  "required": ["notable", "ltas_rms_db", "summary"],
  "additionalProperties": false
}

```

### Examples

**Compare a track with itself** (`song_with_item`)

```json
{
  "reference": "tr_1",
  "test": "tr_1"
}

```

Result:

```json
{
  "notable": [],
  "ltas_rms_db": 0.0,
  "summary": "<any>"
}

```

## `analyze.masking@1`

Estimate masking between two tracks over the same range.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `masker_track` | string |  |  |  |  |
| `maskee_track` | string |  |  |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |

### Result

```json
{}

```

### Examples

**One track against itself** (`song_with_fx`)

```json
{
  "masker_track": "tr_1",
  "maskee_track": "tr_1"
}

```

Result:

```json
"<any>"

```

## `analyze.render@1`

Analyze a render id or audio path: loudness, spectrum, envelope and stereo.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `render` | string |  |  |  |  |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. |

### Result

```json
{}

```

### Examples

**Analyze a mix** (`song_with_render`)

```json
{
  "render": "rd_1"
}

```

Result:

```json
"<any>"

```

## `analyze.track@1`

Render one track's output offline and analyze it.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tap` | string |  |  | "post_fader" |  |
| `detail` | string |  |  | "summary" | summary: key scalars; standard: + percentiles and spectrum; full: + time series. |

### Result

```json
{}

```

### Examples

**Analyze a track** (`song_with_item`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
"<any>"

```

## `batch@1`

Run commands atomically.

Run commands atomically.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `label` | string |  |  |  |  |
| `steps` | array |  |  |  |  |

### Result

```json
{
  "type": "object"
}

```

### Examples

**Empty batch** (`empty`)

```json
{
  "steps": []
}

```

Result:

```json
{
  "label": "",
  "results": []
}

```

## `branch.create@1`

Start a branch from a branch's head (default: the current one) and switch to it.

Agents: work on agent/<you>/<topic> branches, then branch.diff and branch.merge into main. Merging replays your edits onto main even if main moved; conflicting edits are reported.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `from` | string |  |  |  |  |
| `switch` | boolean |  |  | true |  |

### Result

```json
{
  "type": "object",
  "title": "branch_name",
  "properties": {
    "name": {
      "type": "string"
    }
  },
  "required": ["name"],
  "additionalProperties": false
}

```

### Errors

- `branch.exists`: a branch with that name exists Hint: pick another name or switch to it

### Examples

**An agent branch** (`song`)

```json
{
  "name": "agent/claude/darker"
}

```

Result:

```json
{
  "name": "agent/claude/darker"
}

```

## `branch.delete@1`

Delete a branch (not main, not the current one).



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Delete** (`branch_ahead`)

```json
{
  "name": "agent/claude/darker"
}

```

Result:

```json
{}

```

## `branch.diff@1`

What differs between two branch heads: objects added, removed, changed (fields).



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `a` | string |  |  |  |  |
| `b` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "branch_diff",
  "properties": {
    "changes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "diff_entry",
        "properties": {
          "op": {
            "type": "string"
          },
          "id": {
            "type": "string"
          },
          "fields": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["op", "id", "fields"],
        "additionalProperties": false
      }
    }
  },
  "required": ["changes"],
  "additionalProperties": false
}

```

### Examples

**Main against itself** (`song`)

```json
{
  "a": "main",
  "b": "main"
}

```

Result:

```json
{
  "changes": []
}

```

## `branch.list@1`

List branches and the current one.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "branch_list",
  "properties": {
    "branches": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "current": {
      "type": "string"
    }
  },
  "required": ["branches", "current"],
  "additionalProperties": false
}

```

### Examples

**List** (`empty`)

```json
{}

```

Result:

```json
{
  "branches": ["main"],
  "current": "main"
}

```

## `branch.merge@1`

Merge a branch into another (default: main) by replaying its edits.

Fast-forwards when the target has not moved. Otherwise replays the branch's edits; IDs it created are renumbered where needed (see remapped). If both sides changed the same field of an object, nothing is merged and the error lists each conflict.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | string |  |  |  |  |
| `into` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "branch_merge",
  "properties": {
    "fast_forward": {
      "type": "boolean"
    },
    "up_to_date": {
      "type": "boolean"
    },
    "version": {
      "type": "integer"
    },
    "remapped": {}
  },
  "required": ["fast_forward", "up_to_date", "version", "remapped"],
  "additionalProperties": false
}

```

### Errors

- `branch.conflict`: both sides changed the same thing Hint: details: one per conflict with the object

### Examples

**Merge an agent branch** (`branch_ahead`)

```json
{
  "from": "agent/claude/darker"
}

```

Result:

```json
{
  "fast_forward": true,
  "up_to_date": false,
  "version": "<any>",
  "remapped": {}
}

```

## `branch.switch@1`

Make another branch current (edits, undo and playback follow it).



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "branch_name",
  "properties": {
    "name": {
      "type": "string"
    }
  },
  "required": ["name"],
  "additionalProperties": false
}

```

### Examples

**Back to main** (`song`)

```json
{
  "name": "main"
}

```

Result:

```json
{
  "name": "main"
}

```

## `engine.start@1`

Start real-time playback on the audio device (jack) or without one (dummy).

backend: jack (default; PipeWire's JACK or a JACK server) or dummy (no device, paced like one). block: the device block in frames (default 256); the device's period may differ and is reported. anticipative (default true): render ahead of the playhead on worker threads (edits are heard up to 200 ms later); false renders everything in the audio callback. The project must run at the device's rate.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `backend` | string |  |  |  |  |
| `block` | integer |  | 16.0..4096.0 |  |  |
| `anticipative` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "engine_status",
  "properties": {
    "running": {
      "type": "boolean"
    },
    "backend": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "block": {
      "type": "integer"
    },
    "period": {
      "type": "integer"
    },
    "plan_latency": {
      "type": "integer"
    },
    "device_latency": {
      "type": "integer"
    },
    "xruns": {
      "type": "integer"
    },
    "cycles": {
      "type": "integer"
    },
    "process_errors": {
      "type": "integer"
    },
    "restart_needed": {
      "type": "boolean"
    },
    "anticipative": {
      "type": "boolean"
    },
    "anticipation_misses": {
      "type": "integer"
    }
  },
  "required": [
    "running",
    "backend",
    "sample_rate",
    "block",
    "period",
    "plan_latency",
    "device_latency",
    "xruns",
    "cycles",
    "process_errors",
    "restart_needed",
    "anticipative",
    "anticipation_misses"
  ],
  "additionalProperties": false
}

```

### Errors

- `engine.running`: the engine is already running Hint: engine.stop first
- `backend.unavailable`: JACK is not installed Hint: install pipewire-jack
- `backend.open`: no JACK server answered Hint: is PipeWire running?
- `engine.sample_rate`: the project's rate is not the device's Hint: match them

### Examples

**Without a device** (`empty`)

```json
{
  "backend": "dummy",
  "block": 128
}

```

Result:

```json
{
  "running": true,
  "backend": "dummy",
  "sample_rate": 48000.0,
  "block": 128,
  "period": 128,
  "plan_latency": 0,
  "device_latency": 0,
  "xruns": 0,
  "cycles": "<any>",
  "process_errors": 0,
  "restart_needed": false,
  "anticipative": true,
  "anticipation_misses": 0
}

```

## `engine.status@1`

Real-time engine state: backend, rate, block, device period, latency, xruns.

plan_latency: the session's plugin delay in frames; device_latency: frames from the engine's outputs to the device. restart_needed: the device changed rate or went away; engine.stop and engine.start again.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "engine_status",
  "properties": {
    "running": {
      "type": "boolean"
    },
    "backend": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "block": {
      "type": "integer"
    },
    "period": {
      "type": "integer"
    },
    "plan_latency": {
      "type": "integer"
    },
    "device_latency": {
      "type": "integer"
    },
    "xruns": {
      "type": "integer"
    },
    "cycles": {
      "type": "integer"
    },
    "process_errors": {
      "type": "integer"
    },
    "restart_needed": {
      "type": "boolean"
    },
    "anticipative": {
      "type": "boolean"
    },
    "anticipation_misses": {
      "type": "integer"
    }
  },
  "required": [
    "running",
    "backend",
    "sample_rate",
    "block",
    "period",
    "plan_latency",
    "device_latency",
    "xruns",
    "cycles",
    "process_errors",
    "restart_needed",
    "anticipative",
    "anticipation_misses"
  ],
  "additionalProperties": false
}

```

### Examples

**Not running** (`empty`)

```json
{}

```

Result:

```json
{
  "running": false,
  "backend": "",
  "sample_rate": 0.0,
  "block": 0,
  "period": 0,
  "plan_latency": 0,
  "device_latency": 0,
  "xruns": 0,
  "cycles": 0,
  "process_errors": 0,
  "restart_needed": false,
  "anticipative": false,
  "anticipation_misses": 0
}

```

## `engine.stop@1`

Stop real-time playback and release the audio device.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Stop** (`engine_running`)

```json
{}

```

Result:

```json
{}

```

## `fx.add@1`

Insert a plugin into a track's FX chain and return its id.

plugin: id or name from plugin.list; patch: an initial (partial) patch for semantic plugins.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `plugin` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_add",
  "properties": {
    "fx": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "semantic": {
      "type": "boolean"
    },
    "chain": {
      "type": "array",
      "items": {
        "type": "string"
      }
    }
  },
  "required": ["fx", "name", "semantic", "chain"],
  "additionalProperties": false
}

```

### Errors

- `plugin.unknown`: no such plugin Hint: closest ids in the hint; see plugin.list

### Examples

**An instrument on the bass track** (`song`)

```json
{
  "track": "tr_1",
  "plugin": "com.suppleaudio.test.sine"
}

```

Result:

```json
{
  "fx": "fx_1",
  "name": "Test Sine",
  "semantic": false,
  "chain": ["fx_1"]
}

```

## `fx.describe@1`

Describe a plugin instance in words; full adds its parameters and patch.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `full` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_description",
  "properties": {
    "plugin": {
      "type": "string"
    },
    "name": {
      "type": "string"
    },
    "description": {
      "type": "string"
    },
    "params": {},
    "patch": {}
  },
  "required": ["plugin", "name", "description"],
  "additionalProperties": false
}

```

### Examples

**Describe** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "plugin": "com.suppleaudio.test.gain",
  "name": "<any>",
  "description": "<any>"
}

```

## `fx.param.set@1`

Set plugin parameters by key in physical units: {"cutoff": "800 Hz"}.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `values` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "merge_result",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    }
  },
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Examples

**Set the gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "values": {
    "gain": -3
  }
}

```

Result:

```json
{
  "changed": "<any>",
  "description": "<any>"
}

```

## `fx.patch.merge@1`

Merge a partial patch into a plugin instance; returns the effective change.

Semantic plugins only. Values in physical units: {"params": {"gain": "-6 dB"}}.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "merge_result",
  "properties": {
    "changed": {},
    "description": {
      "type": "string"
    }
  },
  "required": ["changed", "description"],
  "additionalProperties": false
}

```

### Examples

**Turn the gain down** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "patch": {
    "params": {
      "gain": "-6 dB"
    }
  }
}

```

Result:

```json
{
  "changed": "<any>",
  "description": "<any>"
}

```

## `fx.remove@1`

Remove a plugin instance (and its envelopes).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Remove** (`song_with_fx`)

```json
{
  "fx": "fx_1"
}

```

Result:

```json
{}

```

## `fx.set@1`

Set instance options: bypass, offline, wet (%), label.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `bypass` | boolean |  |  |  |  |
| `offline` | boolean |  |  |  |  |
| `wet` | value | % | 0.0..100.0 |  | Quantity strings are range-checked by the server. |
| `label` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Half wet** (`song_with_fx`)

```json
{
  "fx": "fx_1",
  "wet": "50%"
}

```

Result:

```json
{}

```

## `history.list@1`

List the current branch's recent versions (newest first).



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_list",
  "properties": {
    "versions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "history_entry",
        "properties": {
          "version": {
            "type": "integer"
          },
          "label": {
            "type": "string"
          },
          "origin": {
            "type": "string"
          }
        },
        "required": ["version", "label", "origin"],
        "additionalProperties": false
      }
    },
    "can_undo": {
      "type": "boolean"
    },
    "can_redo": {
      "type": "boolean"
    }
  },
  "required": ["versions", "can_undo", "can_redo"],
  "additionalProperties": false
}

```

### Examples

**List** (`song`)

```json
{}

```

Result:

```json
{
  "versions": "<any>",
  "can_undo": true,
  "can_redo": false
}

```

## `history.redo@1`

Redo the last undone change.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_result",
  "properties": {
    "version": {
      "type": "integer"
    },
    "label": {
      "type": "string"
    }
  },
  "required": ["version", "label"],
  "additionalProperties": false
}

```

### Examples

**Redo** (`song_undone`)

```json
{}

```

Result:

```json
{
  "version": "<any>",
  "label": "<any>"
}

```

## `history.undo@1`

Undo the last change on the current branch.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "history_result",
  "properties": {
    "version": {
      "type": "integer"
    },
    "label": {
      "type": "string"
    }
  },
  "required": ["version", "label"],
  "additionalProperties": false
}

```

### Examples

**Undo** (`song`)

```json
{}

```

Result:

```json
{
  "version": "<any>",
  "label": "<any>"
}

```

## `item.add@1`

Add an item on a track: MIDI written in notation, or an audio file.

position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h" — every bar must add up; the item is as long as the written bars unless length (beats) is given. audio_file: a path at the project's sample rate.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `position` | number |  | 0.0.. |  | Beats. |
| `bar` | integer |  | 1.0.. |  | Start at this bar (1-based). |
| `length` | number |  |  |  | Beats. |
| `notation` | string |  |  |  |  |
| `audio_file` | string |  |  |  |  |
| `name` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "item_add",
  "properties": {
    "item": {
      "type": "string"
    },
    "take": {
      "type": "string"
    },
    "source": {
      "type": "string"
    },
    "position": {
      "type": "number"
    },
    "length": {
      "type": "number"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "string"
      }
    }
  },
  "required": ["item", "take", "position", "length"],
  "additionalProperties": false
}

```

### Errors

- `notation.*`: the notation does not compile Hint: line:col and a hint in the error
- `tr.unknown`: no such track Hint: see track.list

### Examples

**A bass line at bar 1** (`song`)

```json
{
  "track": "tr_1",
  "bar": 1,
  "notation": "voice bass; 1: C2:q r:q C2:q r:q"
}

```

Result:

```json
{
  "item": "it_1",
  "take": "tk_1",
  "source": "mi_1",
  "position": 0.0,
  "length": 4.0
}

```

## `item.move@1`

Move an item in time (position in beats or bar) and/or to another track.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1". |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0.. |  |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Two bars later** (`song_with_item`)

```json
{
  "item": "it_1",
  "bar": 3
}

```

Result:

```json
{}

```

## `item.remove@1`

Remove an item with its takes (unpooled sources go too).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Remove** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{}

```

## `job.cancel@1`

Cancel a job.

Cancel a job.

Kind: `action`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

### Result

```json
{
  "type": "object"
}

```

### Examples

**Cancel job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "cancelled",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.list@1`

List session jobs.

List session jobs.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object"
}

```

### Examples

**List jobs** (`empty`)

```json
{}

```

Result:

```json
{
  "jobs": []
}

```

## `job.status@1`

Get a job's status.

Get a job's status.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |

### Result

```json
{
  "type": "object"
}

```

### Examples

**Get job** (`job_running`)

```json
{
  "job": "jb_1"
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `job.wait@1`

Wait for a job.

Wait for a job.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `job` | string |  |  |  |  |
| `timeout_s` | number |  |  |  |  |

### Result

```json
{
  "type": "object"
}

```

### Examples

**Wait for job** (`job_running`)

```json
{
  "job": "jb_1",
  "timeout_s": 0
}

```

Result:

```json
{
  "job": "jb_1",
  "status": "running",
  "progress": {
    "fraction": 0.0,
    "message": ""
  }
}

```

## `midi.get@1`

Read an item's MIDI as notation.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1". |

### Result

```json
{
  "type": "object",
  "title": "text",
  "properties": {
    "text": {
      "type": "string"
    }
  },
  "required": ["text"],
  "additionalProperties": false
}

```

### Examples

**Read the bass line** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "text": "<any>"
}

```

## `midi.write@1`

Replace an item's MIDI with notation (the item keeps its position and length).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  | Item id, e.g. "it_1". |
| `notation` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Rewrite the bass line** (`song_with_item`)

```json
{
  "item": "it_1",
  "notation": "voice bass; 1: C2:e C2:e r:q G1:h"
}

```

Result:

```json
{}

```

## `project.get@1`

Show the project: name, rate, tempo, meter, file, version and branch.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "version": {
      "type": "integer"
    }
  },
  "required": ["name", "path", "branch", "sample_rate", "tempo", "meter", "version"],
  "additionalProperties": false
}

```

### Examples

**A new project** (`empty`)

```json
{}

```

Result:

```json
{
  "name": "",
  "path": "",
  "branch": "main",
  "sample_rate": 48000.0,
  "tempo": 120.0,
  "meter": "4/4",
  "version": 0
}

```

## `project.open@1`

Open a project file (replaces the current project and its history).



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "project_info",
  "properties": {
    "name": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "branch": {
      "type": "string"
    },
    "sample_rate": {
      "type": "number"
    },
    "tempo": {
      "type": "number"
    },
    "meter": {
      "type": "string"
    },
    "version": {
      "type": "integer"
    }
  },
  "required": ["name", "path", "branch", "sample_rate", "tempo", "meter", "version"],
  "additionalProperties": false
}

```

### Errors

- `file.open`: the file cannot be read Hint: check the path
- `schema.invalid`: the file has problems Hint: details list each with its path

### Examples

**Open** (`saved`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "name": "<any>",
  "path": "<any>",
  "branch": "main",
  "sample_rate": "<any>",
  "tempo": "<any>",
  "meter": "<any>",
  "version": "<any>"
}

```

## `project.save@1`

Save the project (canonical JSON) to its file or a new path.

path: where to save (becomes the project's file); omit to save in place.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `path` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "path",
  "properties": {
    "path": {
      "type": "string"
    }
  },
  "required": ["path"],
  "additionalProperties": false
}

```

### Examples

**Save next to the work directory** (`empty`)

```json
{
  "path": "example-project.json"
}

```

Result:

```json
{
  "path": "<any>"
}

```

## `project.set@1`

Change project metadata (name, title, author, notes, pan law).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `title` | string |  |  |  |  |
| `author` | string |  |  |  |  |
| `notes` | string |  |  |  |  |
| `pan_law_db` | value | dB | -6.0..0.0 |  | Quantity strings are range-checked by the server. |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Name it** (`empty`)

```json
{
  "name": "Demo"
}

```

Result:

```json
{}

```

## `registry.describe@1`

Describe a command.

Describe a command.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `command` | string |  |  |  |  |

### Result

```json
{
  "type": "object"
}

```

### Examples

**Describe registry.list** (`empty`)

```json
{
  "command": "registry.list"
}

```

Result:

```json
{
  "name": "registry.list",
  "version": 1,
  "kind": "query",
  "exposure": "mcp-exec",
  "summary": "List available commands.",
  "description": "List available commands.",
  "tags": [],
  "args": {
    "type": "object",
    "properties": {
      "tag": {
        "type": "string"
      },
      "kind": {
        "type": "string"
      },
      "text": {
        "type": "string"
      }
    },
    "required": [],
    "additionalProperties": false
  },
  "result": {
    "type": "object"
  },
  "errors": [],
  "examples": [
    {
      "title": "List commands",
      "args": {
        "text": "registry.list"
      },
      "result": {
        "commands": [
          {
            "name": "registry.list",
            "version": 1,
            "kind": "query",
            "summary": "List available commands."
          }
        ]
      },
      "fixture": "empty"
    }
  ]
}

```

## `registry.list@1`

List available commands.

List available commands.

Kind: `query`. Exposure: `mcp-exec`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `tag` | string |  |  |  |  |
| `kind` | string |  |  |  |  |
| `text` | string |  |  |  |  |

### Result

```json
{
  "type": "object"
}

```

### Examples

**List commands** (`empty`)

```json
{
  "text": "registry.list"
}

```

Result:

```json
{
  "commands": [
    {
      "name": "registry.list",
      "version": 1,
      "kind": "query",
      "summary": "List available commands."
    }
  ]
}

```

## `render.mix@1`

Render the mix offline to a float WAV (default: start to the last item's end).

start_beat/end_beat in beats; tail_s seconds after the end (reverb tails). analyze: summary or full returns a supple.analysis-report/1.0 inline. The render id can be passed to analyze.render or analyze.compare.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tail_s` | number |  | 0.0..60.0 |  |  |
| `name` | string |  |  |  |  |
| `analyze` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "render_mix",
  "properties": {
    "render": {
      "type": "string"
    },
    "path": {
      "type": "string"
    },
    "seconds": {
      "type": "number"
    },
    "peak_dbfs": {
      "type": "number"
    },
    "output_latency": {
      "type": "integer"
    },
    "warnings": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "analysis": {}
  },
  "required": ["render", "path", "seconds", "peak_dbfs", "output_latency"],
  "additionalProperties": false
}

```

### Examples

**Render** (`song_with_fx`)

```json
{
  "name": "mix-v1"
}

```

Result:

```json
{
  "render": "rd_1",
  "path": "<any>",
  "seconds": "<any>",
  "peak_dbfs": "<any>",
  "output_latency": 0
}

```

## `session.describe@1`

Describe the whole session in compact text (tracks, items, fx, routes).

The quickest way to see where things are; use the IDs it shows with other commands.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "text",
  "properties": {
    "text": {
      "type": "string"
    }
  },
  "required": ["text"],
  "additionalProperties": false
}

```

### Examples

**Describe** (`song`)

```json
{}

```

Result:

```json
{
  "text": "<any>"
}

```

## `tempo.set@1`

Replace the tempo map: points {beat, bpm, ramp?}; the first must be at beat 0.

ramp: true changes the tempo linearly (in beats) to the next point; otherwise it steps.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `points` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "count",
  "properties": {
    "count": {
      "type": "integer"
    }
  },
  "required": ["count"],
  "additionalProperties": false
}

```

### Examples

**Slow down after 8 bars** (`empty`)

```json
{
  "points": [
    {
      "beat": 0,
      "bpm": 120
    },
    {
      "beat": 32,
      "bpm": 100,
      "ramp": false
    }
  ]
}

```

Result:

```json
{
  "count": 2
}

```

## `timesig.set@1`

Set the meter from a bar on, e.g. {"bar": 9, "meter": "3/4"}.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `bar` | integer |  | 1.0.. |  |  |
| `meter` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Waltz from bar 9** (`empty`)

```json
{
  "bar": 9,
  "meter": "3/4"
}

```

Result:

```json
{}

```

## `track.add@1`

Add a track (under the master, or inside a folder track) and return its id.

parent: a track id to nest under (it becomes a folder); index: position among its siblings.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `name` | string |  |  |  |  |
| `parent` | string |  |  |  | Parent track id, e.g. "tr_1". |
| `index` | integer |  | 0.0.. |  |  |
| `channels` | integer |  | 1.0..64.0 |  |  |

### Result

```json
{
  "type": "object",
  "title": "track_ref",
  "properties": {
    "track": {
      "type": "string"
    }
  },
  "required": ["track"],
  "additionalProperties": false
}

```

### Examples

**Add a bass track** (`empty`)

```json
{
  "name": "Bass"
}

```

Result:

```json
{
  "track": "tr_1"
}

```

## `track.list@1`

List tracks in tree order with their level, mute/solo, fx and items.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "track_list",
  "properties": {
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "track_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "parent": {
            "type": "string"
          },
          "depth": {
            "type": "integer"
          },
          "volume_db": {
            "type": "number"
          },
          "mute": {
            "type": "boolean"
          },
          "solo": {
            "type": "string"
          },
          "fx": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "items": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["id", "name", "parent", "depth", "volume_db", "mute", "solo", "fx", "items"],
        "additionalProperties": false
      }
    }
  },
  "required": ["tracks"],
  "additionalProperties": false
}

```

### Examples

**List** (`song`)

```json
{}

```

Result:

```json
{
  "tracks": "<any>"
}

```

## `track.remove@1`

Remove a track with its items, fx, routes and envelopes (not a folder).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Remove** (`song`)

```json
{
  "track": "tr_1"
}

```

Result:

```json
{}

```

## `track.set@1`

Set track fields: name, volume_db, pan, width, mute, solo, color, notes.

Only the given fields change. solo: off | on | in_place | defeat.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |
| `notes` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `width` | value | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |
| `phase_invert` | boolean |  |  |  |  |
| `solo` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Examples

**Turn the bass down 4 dB and a little left** (`song`)

```json
{
  "track": "tr_1",
  "volume_db": "-4 dB",
  "pan": -15
}

```

Result:

```json
{}

```

## `transport.get@1`

The play position and whether it is playing.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    }
  },
  "required": ["playing", "seconds", "beat", "sample"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**Stopped at the start** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "seconds": 0.0,
  "beat": 0.0,
  "sample": 0
}

```

## `transport.pause@1`

Pause playback where it is.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    }
  },
  "required": ["playing", "seconds", "beat", "sample"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>"
}

```

## `transport.play@1`

Start playback from the current position.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    }
  },
  "required": ["playing", "seconds", "beat", "sample"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": true,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>"
}

```

## `transport.seek@1`

Move the play position (one of seconds, beat or bar).



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `seconds` | number |  | 0.0.. |  |  |
| `beat` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    }
  },
  "required": ["playing", "seconds", "beat", "sample"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `transport.position`: not exactly one position Hint: give seconds, beat or bar

### Examples

**To bar 2 (120 BPM, 4/4)** (`engine_running`)

```json
{
  "bar": 2
}

```

Result:

```json
{
  "playing": false,
  "seconds": 2.0,
  "beat": 4.0,
  "sample": 96000
}

```

## `transport.stop@1`

Stop playback and return to where it started.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "transport",
  "properties": {
    "playing": {
      "type": "boolean"
    },
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "sample": {
      "type": "integer"
    }
  },
  "required": ["playing", "seconds", "beat", "sample"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>"
}

```
