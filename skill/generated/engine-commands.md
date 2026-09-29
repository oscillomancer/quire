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

## `branch.render-ab@1`

Render two branch heads over the same range and compare them.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `a` | string |  |  |  |  |
| `b` | string |  |  |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tail_s` | number |  | 0.0..600.0 |  |  |

### Result

```json
{}

```

### Examples

**Compare main with itself** (`song`)

```json
{
  "a": "main",
  "b": "main",
  "end_beat": 1
}

```

Result:

```json
{
  "a": "<any>",
  "b": "<any>",
  "comparison": "<any>"
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

backend: jack (default; PipeWire's JACK or a JACK server) or dummy (no device, paced like one). block: the device block in frames (default 256); the device's period may differ and is reported. anticipative (default true): render ahead of the playhead on worker threads (edits are heard up to 200 ms later); false renders everything in the audio callback. The engine runs at the device's rate (media is resampled; renders stay at the project's rate).

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
    },
    "failed_fx": {
      "type": "array",
      "items": {
        "type": "string"
      }
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
    "anticipation_misses",
    "failed_fx"
  ],
  "additionalProperties": false
}

```

### Errors

- `engine.running`: the engine is already running Hint: engine.stop first
- `backend.unavailable`: JACK is not installed Hint: install pipewire-jack
- `backend.open`: no JACK server answered Hint: is PipeWire running?

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
  "anticipation_misses": 0,
  "failed_fx": []
}

```

## `engine.status@1`

Real-time engine state: backend, rate, block, device period, latency, xruns.

plan_latency: the session's plugin delay in frames; device_latency: frames from the engine's outputs to the device. restart_needed: the device changed rate or went away; engine.stop and engine.start again. failed_fx: isolated plugins whose process crashed or hung (silent now; fx.restart re-creates them).

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
    },
    "failed_fx": {
      "type": "array",
      "items": {
        "type": "string"
      }
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
    "anticipation_misses",
    "failed_fx"
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
  "anticipation_misses": 0,
  "failed_fx": []
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

## `env.add@1`

Automate a parameter: a track's or route's mixer control, or a plugin's.

Target one of track (param volume|trim|pan|width|mute), route (volume|pan|mute) or fx (a semantic key such as "gain" or "filter1.cutoff", or for other plugins a CLAP id or parameter name). points: [{beat|bar|seconds, value, shape?, tension?}]; values in the parameter's unit (dB, %, Hz… — text like "800 Hz" works for plugins); shapes linear, square, slow, fast_start, fast_end, bezier. Plugin values move along the parameter's curve (a log cutoff sweeps evenly in pitch).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `route` | string |  |  |  | Route id, e.g. "sd_1". |
| `param` | string |  |  |  |  |
| `points` | array |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "env_add",
  "properties": {
    "env": {
      "type": "string"
    },
    "points": {
      "type": "integer"
    }
  },
  "required": ["env", "points"],
  "additionalProperties": false
}

```

### Examples

**Fade a track out over bar 2** (`song`)

```json
{
  "track": "tr_1",
  "param": "volume",
  "points": [
    {
      "bar": 2,
      "value": 0
    },
    {
      "bar": 3,
      "value": "-60 dB"
    }
  ]
}

```

Result:

```json
{
  "env": "en_1",
  "points": 2
}

```

**Sweep a plugin parameter** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "param": "gain",
  "points": [
    {
      "beat": 0,
      "value": "-12 dB",
      "shape": "slow"
    },
    {
      "beat": 4,
      "value": 0
    }
  ]
}

```

Result:

```json
{
  "env": "en_1",
  "points": 2
}

```

## `env.list@1`

List envelopes (optionally of one track, route or plugin).



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `object` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "env_list",
  "properties": {
    "envelopes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "env_entry",
        "properties": {
          "env": {
            "type": "string"
          },
          "target": {
            "type": "string"
          },
          "param": {
            "type": "string"
          },
          "points": {
            "type": "integer"
          },
          "active": {
            "type": "boolean"
          },
          "mode": {
            "type": "string"
          }
        },
        "required": ["env", "target", "param", "points", "active", "mode"],
        "additionalProperties": false
      }
    }
  },
  "required": ["envelopes"],
  "additionalProperties": false
}

```

### Examples

**All** (`song_with_env`)

```json
{}

```

Result:

```json
{
  "envelopes": [
    {
      "env": "en_1",
      "target": "fx_2",
      "param": "gain",
      "points": 2,
      "active": true,
      "mode": "read"
    }
  ]
}

```

## `env.points.set@1`

Replace an envelope's points (all of them, or those in a beat range).

from_beat/to_beat: replace only points with from_beat <= beat < to_beat.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `points` | array |  |  |  |  |
| `from_beat` | number |  | 0.0.. |  |  |
| `to_beat` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "env_count",
  "properties": {
    "points": {
      "type": "integer"
    }
  },
  "required": ["points"],
  "additionalProperties": false
}

```

### Examples

**Three points** (`song_with_env`)

```json
{
  "env": "en_1",
  "points": [
    {
      "beat": 0,
      "value": -6
    },
    {
      "beat": 2,
      "value": 0,
      "shape": "square"
    },
    {
      "beat": 3,
      "value": "-3 dB"
    }
  ]
}

```

Result:

```json
{
  "points": 3
}

```

## `env.remove@1`

Remove an envelope (the parameter returns to its set value).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |

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

**Remove** (`song_with_env`)

```json
{
  "env": "en_1"
}

```

Result:

```json
{}

```

## `env.set@1`

Set an envelope's options: active (heard), mode (read, touch, latch, write).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `env` | string |  |  |  | Envelope id, e.g. "en_1". |
| `active` | boolean |  |  |  |  |
| `mode` | string |  |  |  |  |

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

**Mute the automation** (`song_with_env`)

```json
{
  "env": "en_1",
  "active": false
}

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
| `filter` | string |  |  |  | Plain parameters only: list those whose name contains this text (case-insensitive). |

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
    "patch": {},
    "params_total": {
      "type": "integer",
      "description": "Plain parameters matching; at most 512 are listed."
    }
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

## `fx.move@1`

Move a plugin within a track chain or to another track.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `track` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |

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

**Move a gain first** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "index": 0
}

```

Result:

```json
{}

```

## `fx.param.get@1`

Read one saved plugin parameter by semantic key or numeric CLAP id.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `param` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_param_result",
  "properties": {
    "value": {}
  },
  "required": ["value"],
  "additionalProperties": false
}

```

### Examples

**Read gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "param": "gain"
}

```

Result:

```json
{
  "value": "<any>"
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

## `fx.patch.get@1`

Read a semantic plugin's saved patch.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `changed_only` | boolean |  |  | false |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {}
  },
  "required": ["patch"],
  "additionalProperties": false
}

```

### Examples

**Read gain** (`song_with_fx`)

```json
{
  "fx": "fx_2"
}

```

Result:

```json
{
  "patch": "<any>"
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

## `fx.patch.set@1`

Replace a semantic plugin's complete patch.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  |  |
| `patch` | value |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "fx_patch_result",
  "properties": {
    "patch": {}
  },
  "required": ["patch"],
  "additionalProperties": false
}

```

### Examples

**Replace gain** (`song_with_fx`)

```json
{
  "fx": "fx_2",
  "patch": {
    "schema": "supple.test-gain-patch/1.0",
    "params": {
      "gain": -6
    }
  }
}

```

Result:

```json
{
  "patch": "<any>"
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

## `fx.restart@1`

Re-create a plugin instance from its saved patch/state (after a crash).

engine.status lists failed_fx: isolated plugins whose process crashed or hung. The new instance starts from the model's patch or state.

Kind: `action`. Exposure: `mcp-tool`.

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

**Restart** (`song_with_fx`)

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

Set instance options: bypass, offline, wet (%), label, isolation.

isolation: in_process, dedicated (the plugin runs in a process of its own: a crash silences only it), shared_bridge (one process for all such plugins), inherit (the project default: in_process).

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `fx` | string |  |  |  | Plugin instance id, e.g. "fx_1". |
| `bypass` | boolean |  |  |  |  |
| `offline` | boolean |  |  |  |  |
| `wet` | value | % | 0.0..100.0 |  | Quantity strings are range-checked by the server. |
| `label` | string |  |  |  |  |
| `isolation` | string |  |  |  |  |

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

## `item.duplicate@1`

Copy an item and its takes to a new beat position (default: immediately after).



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `to` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Examples

**Repeat** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "id": "it_2"
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

## `item.set@1`

Set an item's position, length, playback, fades, name, or color.

Positions and lengths are in beats; fades are in seconds. Name is the active take's name.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `position` | number |  | 0.0.. |  |  |
| `bar` | integer |  | 1.0.. |  |  |
| `length` | number |  | 0.0.. |  |  |
| `mute` | boolean |  |  |  |  |
| `loop` | boolean |  |  |  |  |
| `gain_db` | value | dB | -150.0..24.0 |  | Quantity strings are range-checked by the server. |
| `fade_in` | number |  | 0.0.. |  |  |
| `fade_out` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

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

**Lower an item** (`song_with_item`)

```json
{
  "item": "it_1",
  "gain_db": "-3 dB"
}

```

Result:

```json
{}

```

## `item.split@1`

Split an audio or MIDI item at a beat, keeping take source positions aligned.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `at` | number |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "item_split_result",
  "properties": {
    "left": {
      "type": "string"
    },
    "right": {
      "type": "string"
    }
  },
  "required": ["left", "right"],
  "additionalProperties": false
}

```

### Examples

**Split halfway** (`song_with_item`)

```json
{
  "item": "it_1",
  "at": 2
}

```

Result:

```json
{
  "left": "it_1",
  "right": "it_2"
}

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

## `job.get@1`

Get a job's status and progress.

Get a job's status and progress.

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

**Get render job** (`job_running`)

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

## `marker.add@1`

Add a named marker in beats.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `pos` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Examples

**Add** (`empty`)

```json
{
  "pos": 4,
  "name": "Verse"
}

```

Result:

```json
{
  "id": "mk_1"
}

```

## `marker.list@1`

List rulers in this project.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "ruler_list",
  "properties": {
    "markers": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "ruler_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "color": {
            "type": "string"
          },
          "pos": {
            "type": "number"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          }
        },
        "required": ["id", "name", "color"],
        "additionalProperties": false
      }
    }
  },
  "required": ["markers"],
  "additionalProperties": false
}

```

### Examples

**List empty** (`empty`)

```json
{}

```

Result:

```json
{
  "markers": []
}

```

## `marker.remove@1`

Remove a ruler.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |

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

**Remove** (`song_with_marker`)

```json
{
  "id": "mk_1"
}

```

Result:

```json
{}

```

## `marker.set@1`

Edit a ruler's position, name, or color.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |
| `pos` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

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

**Rename** (`song_with_marker`)

```json
{
  "id": "mk_1",
  "name": "Bridge"
}

```

Result:

```json
{}

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

## `plugin.list@1`

List scanned plugins; filter matches id, name, or vendor.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `filter` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "plugin_list",
  "properties": {
    "plugins": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "plugin_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "vendor": {
            "type": "string"
          },
          "format": {
            "type": "string",
            "description": "clap or vst3"
          },
          "instrument": {
            "type": "boolean"
          },
          "semantic": {
            "type": "boolean"
          }
        },
        "required": ["id", "name", "vendor", "format", "instrument", "semantic"],
        "additionalProperties": false
      }
    }
  },
  "required": ["plugins"],
  "additionalProperties": false
}

```

### Examples

**Find a test synth** (`empty`)

```json
{
  "filter": "test.sine"
}

```

Result:

```json
{
  "plugins": "<any>"
}

```

## `plugin.scan@1`

Rescan configured plugin paths and return the available plugins.



Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "plugin_list",
  "properties": {
    "plugins": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "plugin_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "vendor": {
            "type": "string"
          },
          "format": {
            "type": "string",
            "description": "clap or vst3"
          },
          "instrument": {
            "type": "boolean"
          },
          "semantic": {
            "type": "boolean"
          }
        },
        "required": ["id", "name", "vendor", "format", "instrument", "semantic"],
        "additionalProperties": false
      }
    }
  },
  "required": ["plugins"],
  "additionalProperties": false
}

```

### Examples

**Scan** (`empty`)

```json
{}

```

Result:

```json
{
  "plugins": "<any>"
}

```

## `project.get@1`

Show the project: name, rate, tempo, meter, file, version and branch.

recoverable: recordings a crash left out of the project (project.recoverable).

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
    },
    "recoverable": {
      "type": "integer"
    }
  },
  "required": ["name", "path", "branch", "sample_rate", "tempo", "meter", "version", "recoverable"],
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
  "version": 0,
  "recoverable": 0
}

```

## `project.open@1`

Open a project file (replaces the current project and its history).

recoverable > 0: recordings a crash left out of the project; project.recoverable lists them and project.recover puts them back.

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
    },
    "recoverable": {
      "type": "integer"
    }
  },
  "required": ["name", "path", "branch", "sample_rate", "tempo", "meter", "version", "recoverable"],
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
  "version": "<any>",
  "recoverable": 0
}

```

## `project.recover@1`

Put recordings a crash left out back into the project (one undo step).

files: as project.recoverable lists them (omit for all). Each becomes an item on the track it was recorded on (or track) at the position it was recorded.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `files` | array |  |  |  |  |
| `track` | string |  |  |  | Track id, e.g. "tr_1". |

### Result

```json
{
  "type": "object",
  "title": "recover_result",
  "properties": {
    "recovered": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["recovered"],
  "additionalProperties": false
}

```

### Errors

- `recover.nothing`: there is nothing to recover Hint: project.recoverable
- `recover.unknown_file`: a file is not a recording to recover Hint: project.recoverable lists them
- `recover.duplicate_file`: a file is listed twice Hint: list each once
- `recover.no_track`: the track it was recorded on is gone Hint: give track

### Examples

**Recover everything** (`recording_interrupted`)

```json
{}

```

Result:

```json
{
  "recovered": [
    {
      "track": "tr_1",
      "item": "it_1",
      "take": "tk_1",
      "file": "<any>",
      "seconds": 1.0,
      "lost_frames": 0,
      "takes": ["tk_1"]
    }
  ]
}

```

## `project.recoverable@1`

List recordings a crash left out of the project.

Takes recorded but never committed (the program stopped mid-recording) or never saved, found in the media folder's rec/. start_seconds is where each was recorded; seconds includes audio written after the file's last header update.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "recoverable_list",
  "properties": {
    "recordings": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recoverable_recording",
        "properties": {
          "file": {
            "type": "string"
          },
          "track": {
            "type": "string"
          },
          "start_seconds": {
            "type": "number"
          },
          "seconds": {
            "type": "number"
          },
          "channels": {
            "type": "integer"
          }
        },
        "required": ["file", "track", "start_seconds", "seconds", "channels"],
        "additionalProperties": false
      }
    }
  },
  "required": ["recordings"],
  "additionalProperties": false
}

```

### Examples

**After a crash while recording** (`recording_interrupted`)

```json
{}

```

Result:

```json
{
  "recordings": [
    {
      "file": "rec/Vocal-1.wav",
      "track": "tr_1",
      "start_seconds": 1.0,
      "seconds": 1.0,
      "channels": 2
    }
  ]
}

```

**Nothing to recover** (`empty`)

```json
{}

```

Result:

```json
{
  "recordings": []
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
| `render` | value |  |  |  |  |

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

## `region.add@1`

Add a named region in beats.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Examples

**Add** (`empty`)

```json
{
  "start": 0,
  "end": 4,
  "name": "Verse"
}

```

Result:

```json
{
  "id": "rg_1"
}

```

## `region.list@1`

List rulers in this project.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "ruler_list",
  "properties": {
    "regions": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "ruler_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "color": {
            "type": "string"
          },
          "pos": {
            "type": "number"
          },
          "start": {
            "type": "number"
          },
          "end": {
            "type": "number"
          }
        },
        "required": ["id", "name", "color"],
        "additionalProperties": false
      }
    }
  },
  "required": ["regions"],
  "additionalProperties": false
}

```

### Examples

**List empty** (`empty`)

```json
{}

```

Result:

```json
{
  "regions": []
}

```

## `region.remove@1`

Remove a ruler.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |

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

**Remove** (`song_with_region`)

```json
{
  "id": "rg_1"
}

```

Result:

```json
{}

```

## `region.set@1`

Edit a ruler's position, name, or color.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `id` | string |  |  |  |  |
| `start` | number |  | 0.0.. |  |  |
| `end` | number |  | 0.0.. |  |  |
| `name` | string |  |  |  |  |
| `color` | string |  |  |  |  |

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

**Rename** (`song_with_region`)

```json
{
  "id": "rg_1",
  "name": "Bridge"
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

## `render.dry-run@1`

List the files a render would write, with lengths, without rendering.

Files go to the renders directory, named by pattern. job: true runs the render as a job (job.get for progress, job.cancel to stop); results carry a render id and an analysis summary per file.

Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `branch` | string |  |  |  |  |
| `pattern` | string |  |  |  | File name pattern (session-model §7.3), e.g. "{project}-{track_number:02}-{track|lower}" |
| `name` | string |  |  |  | A fixed file name instead of a pattern |
| `mode` | string |  |  |  | Stems: isolated (default; the track alone, no master processing) or through_master (the others muted) |
| `tap` | string |  |  |  | Stems: post_fader (default), pre_fader or pre_fx |
| `format` | string |  |  |  | wav (FLAC is not available) |
| `normalize` | value |  |  |  | none, lufs_i, lufs_s, lufs_m, peak, true_peak or rms; or {mode, target, limit_true_peak_db} |
| `operation` | string |  |  |  | Dry run: mix, stems or regions (default from the other arguments) |
| `tracks` | array |  |  |  | Stems: track ids (default: every top-level track) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the project render matrix) |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tail_s` | number |  | 0.0..600.0 |  |  |
| `target` | number |  |  |  |  |
| `limit_true_peak_db` | number |  |  |  |  |
| `bit_depth` | integer |  | 16.0..32.0 |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  |  |
| `channels` | integer |  | 1.0..2.0 |  |  |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**Render project** (`song`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
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
| `pattern` | string |  |  |  |  |
| `branch` | string |  |  |  |  |
| `format` | string |  |  |  |  |
| `bit_depth` | integer |  | 16.0..32.0 |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  |  |
| `channels` | integer |  | 1.0..2.0 |  |  |
| `normalize` | value |  |  |  |  |
| `job` | boolean |  |  | false |  |
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
    "analysis": {},
    "job": {
      "type": "string"
    },
    "status": {
      "type": "string"
    }
  },
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

## `render.regions@1`

Render each region (or the render matrix's tracks per region) to files.

Files go to the renders directory, named by pattern. job: true runs the render as a job (job.get for progress, job.cancel to stop); results carry a render id and an analysis summary per file.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `branch` | string |  |  |  |  |
| `pattern` | string |  |  |  | File name pattern (session-model §7.3), e.g. "{project}-{track_number:02}-{track|lower}" |
| `name` | string |  |  |  | A fixed file name instead of a pattern |
| `mode` | string |  |  |  | Stems: isolated (default; the track alone, no master processing) or through_master (the others muted) |
| `tap` | string |  |  |  | Stems: post_fader (default), pre_fader or pre_fx |
| `format` | string |  |  |  | wav (FLAC is not available) |
| `normalize` | value |  |  |  | none, lufs_i, lufs_s, lufs_m, peak, true_peak or rms; or {mode, target, limit_true_peak_db} |
| `operation` | string |  |  |  | Dry run: mix, stems or regions (default from the other arguments) |
| `tracks` | array |  |  |  | Stems: track ids (default: every top-level track) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the project render matrix) |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tail_s` | number |  | 0.0..600.0 |  |  |
| `target` | number |  |  |  |  |
| `limit_true_peak_db` | number |  |  |  |  |
| `bit_depth` | integer |  | 16.0..32.0 |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  |  |
| `channels` | integer |  | 1.0..2.0 |  |  |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**Render project** (`song_with_region`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```

## `render.stems@1`

Render each selected track to its own file (stems).

Files go to the renders directory, named by pattern. job: true runs the render as a job (job.get for progress, job.cancel to stop); results carry a render id and an analysis summary per file.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `branch` | string |  |  |  |  |
| `pattern` | string |  |  |  | File name pattern (session-model §7.3), e.g. "{project}-{track_number:02}-{track|lower}" |
| `name` | string |  |  |  | A fixed file name instead of a pattern |
| `mode` | string |  |  |  | Stems: isolated (default; the track alone, no master processing) or through_master (the others muted) |
| `tap` | string |  |  |  | Stems: post_fader (default), pre_fader or pre_fx |
| `format` | string |  |  |  | wav (FLAC is not available) |
| `normalize` | value |  |  |  | none, lufs_i, lufs_s, lufs_m, peak, true_peak or rms; or {mode, target, limit_true_peak_db} |
| `operation` | string |  |  |  | Dry run: mix, stems or regions (default from the other arguments) |
| `tracks` | array |  |  |  | Stems: track ids (default: every top-level track) |
| `regions` | array |  |  |  | Regions: region ids (default: all, with the project render matrix) |
| `start_beat` | number |  | 0.0.. |  |  |
| `end_beat` | number |  |  |  |  |
| `tail_s` | number |  | 0.0..600.0 |  |  |
| `target` | number |  |  |  |  |
| `limit_true_peak_db` | number |  |  |  |  |
| `bit_depth` | integer |  | 16.0..32.0 |  |  |
| `sample_rate` | integer |  | 8000.0..384000.0 |  |  |
| `channels` | integer |  | 1.0..2.0 |  |  |
| `job` | boolean |  |  | false |  |

### Result

```json
{}

```

### Examples

**Render project** (`song`)

```json
{
  "end_beat": 1,
  "pattern": "example-{counter:02}"
}

```

Result:

```json
{
  "files": "<any>"
}

```

## `route.add@1`

Send one track to another track, bus, or the master.

The destination must not feed the source. Master accepts routes. Hardware and plugin sidechain routing are not supported yet.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  |  |
| `tap` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Errors

- `route.cycle`: the send would feed back Hint: choose another destination

### Examples

**Send a track to the master** (`song`)

```json
{
  "from": "tr_1",
  "to": "tr_0",
  "volume_db": "-6 dB"
}

```

Result:

```json
{
  "id": "sd_1"
}

```

## `route.list@1`

List sends with endpoints and mixer settings.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |

### Result

```json
{
  "type": "object",
  "title": "route_list",
  "properties": {
    "routes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "route_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "from": {
            "type": "string"
          },
          "to": {
            "type": "string"
          },
          "tap": {
            "type": "string"
          },
          "volume_db": {
            "type": "number"
          },
          "pan": {
            "type": "number"
          },
          "mute": {
            "type": "boolean"
          }
        },
        "required": ["id", "from", "to", "tap", "volume_db", "pan", "mute"],
        "additionalProperties": false
      }
    }
  },
  "required": ["routes"],
  "additionalProperties": false
}

```

### Examples

**No sends yet** (`empty`)

```json
{}

```

Result:

```json
{
  "routes": []
}

```

## `route.remove@1`

Remove a send.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |

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

**Remove** (`song_with_route`)

```json
{
  "route": "sd_1"
}

```

Result:

```json
{}

```

## `route.set@1`

Change a route's endpoints, tap, level, pan, or mute.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `route` | string |  |  |  |  |
| `from` | string |  |  |  |  |
| `to` | string |  |  |  |  |
| `tap` | string |  |  |  |  |
| `volume_db` | value | dB | -150.0..12.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `mute` | boolean |  |  |  |  |

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

**Mute a route** (`song_with_route`)

```json
{
  "route": "sd_1",
  "mute": true
}

```

Result:

```json
{}

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

## `take.activate@1`

Choose the take heard from an item.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `take` | string |  |  |  |  |

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

**Activate** (`song_with_item`)

```json
{
  "item": "it_1",
  "take": "tk_1"
}

```

Result:

```json
{}

```

## `take.add@1`

Add a take to an item, optionally copied from another take of that item.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |
| `from` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "created_id",
  "properties": {
    "id": {
      "type": "string"
    }
  },
  "required": ["id"],
  "additionalProperties": false
}

```

### Examples

**Copy a take** (`song_with_item`)

```json
{
  "item": "it_1",
  "from": "tk_1"
}

```

Result:

```json
{
  "id": "tk_2"
}

```

## `take.list@1`

List the takes of an item and identify its active take.



Kind: `query`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `item` | string |  |  |  |  |

### Result

```json
{
  "type": "object",
  "title": "take_list",
  "properties": {
    "takes": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "take_entry",
        "properties": {
          "id": {
            "type": "string"
          },
          "name": {
            "type": "string"
          },
          "source": {
            "type": "string"
          },
          "active": {
            "type": "boolean"
          },
          "gain_db": {
            "type": "number"
          },
          "pan": {
            "type": "number"
          },
          "start_offset": {
            "type": "number"
          }
        },
        "required": ["id", "name", "source", "active", "gain_db", "pan", "start_offset"],
        "additionalProperties": false
      }
    }
  },
  "required": ["takes"],
  "additionalProperties": false
}

```

### Examples

**List** (`song_with_item`)

```json
{
  "item": "it_1"
}

```

Result:

```json
{
  "takes": "<any>"
}

```

## `take.set@1`

Set take gain, pan, channel mode, name, or source offset.



Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `take` | string |  |  |  |  |
| `gain_db` | value | dB | -150.0..24.0 |  | Quantity strings are range-checked by the server. |
| `pan` | value | % | -100.0..100.0 |  | Quantity strings are range-checked by the server. |
| `channel_mode` | string |  |  |  |  |
| `name` | string |  |  |  |  |
| `start_offset` | number |  |  |  | Source position at the item start; negative for a late first pass. |

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

**Lower a take** (`song_with_item`)

```json
{
  "take": "tk_1",
  "gain_db": "-3 dB"
}

```

Result:

```json
{}

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

## `time.convert@1`

Convert a seconds, beat, or bar position to all three time domains.



Kind: `query`. Exposure: `mcp-tool`.

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
  "title": "time_position",
  "properties": {
    "seconds": {
      "type": "number"
    },
    "beat": {
      "type": "number"
    },
    "bar": {
      "type": "integer"
    },
    "beat_in_bar": {
      "type": "integer"
    },
    "fraction": {
      "type": "number"
    }
  },
  "required": ["seconds", "beat", "bar", "beat_in_bar", "fraction"],
  "additionalProperties": false
}

```

### Examples

**Bar two** (`empty`)

```json
{
  "bar": 2
}

```

Result:

```json
{
  "seconds": 2,
  "beat": 4,
  "bar": 2,
  "beat_in_bar": 1,
  "fraction": 0
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

## `track.move@1`

Move a track under another track or reorder it among its siblings.

parent defaults to the track's current parent (a pure reorder); index defaults to the end of the destination's children. A track cannot move under itself or one of its own descendants.

Kind: `edit`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `track` | string |  |  |  |  |
| `parent` | string |  |  |  |  |
| `index` | integer |  | 0.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "empty",
  "properties": {},
  "additionalProperties": false
}

```

### Errors

- `tr.cycle`: the destination is inside the track Hint: choose another parent

### Examples

**Move a track to the master** (`song`)

```json
{
  "track": "tr_1",
  "parent": "tr_0"
}

```

Result:

```json
{}

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

Set track fields: name, volume_db, pan, width, mute, solo, color, notes, arming.

Only the given fields change. solo: off | on | in_place | defeat. armed + record_input (device input channels, 1-based) prepare a track for transport.record; record_midi records MIDI instead: a device name (substring of the MIDI port, "*" = every device) with midi_channel 1-16 (0 = all). monitor: off | on | auto (auto: while armed) — a monitored track hears its input live (an instrument plays the keyboard), also while stopped. record_mode: input (new items), midi_overdub (merge into the MIDI item under the start, as a new take), midi_replace.

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
| `armed` | boolean |  |  |  |  |
| `record_input` | array |  |  |  |  |
| `record_midi` | string |  |  |  |  |
| `midi_channel` | integer |  | 0.0..16.0 |  |  |
| `record_mode` | string |  |  |  |  |
| `monitor` | string |  |  |  |  |

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

**Arm for recording from inputs 1 and 2** (`song`)

```json
{
  "track": "tr_1",
  "armed": true,
  "record_input": [1, 2]
}

```

Result:

```json
{}

```

**Play and record a MIDI keyboard (any device, channel 1)** (`song`)

```json
{
  "track": "tr_1",
  "armed": true,
  "record_midi": "*",
  "midi_channel": 1,
  "monitor": "auto"
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
    "recording": {
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
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["playing", "recording", "seconds", "beat", "sample", "loop", "punch"],
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
  "recording": false,
  "seconds": 0.0,
  "beat": 0.0,
  "sample": 0,
  "loop": {
    "enabled": false,
    "start_seconds": 0.0,
    "end_seconds": 0.0
  },
  "punch": {
    "enabled": false,
    "start_seconds": 0.0,
    "end_seconds": 0.0
  }
}

```

## `transport.loop@1`

Set or toggle the loop range: playback reaching its end repeats it.

start and end as seconds, beat or bar (start_bar 5, end_bar 9 = bars 5-8); a range turns the loop on unless enabled says otherwise; enabled alone toggles the current range. Recording while looping makes one take per pass on one item. Transport state: not an undo step, not saved in the project.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `start_seconds` | number |  | 0.0.. |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `start_bar` | integer |  | 1.0.. |  |  |
| `end_seconds` | number |  | 0.0.. |  |  |
| `end_beat` | number |  | 0.0.. |  |  |
| `end_bar` | integer |  | 1.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "loop",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "start_seconds": {
      "type": "number"
    },
    "end_seconds": {
      "type": "number"
    }
  },
  "required": ["enabled", "start_seconds", "end_seconds"],
  "additionalProperties": false
}

```

### Errors

- `transport.range`: not one start and one end, or end not after start Hint: e.g. start_bar 5, end_bar 9

### Examples

**Bars 2-3 (120 BPM, 4/4)** (`empty`)

```json
{
  "start_bar": 2,
  "end_bar": 4
}

```

Result:

```json
{
  "enabled": true,
  "start_seconds": 2.0,
  "end_seconds": 6.0
}

```

## `transport.pause@1`

Pause playback where it is (ends a recording).



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
    "recording": {
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
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["playing", "recording", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>"
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
    "recording": {
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
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["playing", "recording", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": true,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>"
}

```

## `transport.punch@1`

Set or toggle the punch range: recordings keep only that range.

start and end as seconds, beat or bar; a range turns punch on unless enabled says otherwise. The recording keeps running; the committed items cover only the punch range (the file keeps everything). Not an undo step.

Kind: `action`. Exposure: `mcp-tool`.

### Arguments

| Field | Type | Unit | Range | Default | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | boolean |  |  |  |  |
| `start_seconds` | number |  | 0.0.. |  |  |
| `start_beat` | number |  | 0.0.. |  |  |
| `start_bar` | integer |  | 1.0.. |  |  |
| `end_seconds` | number |  | 0.0.. |  |  |
| `end_beat` | number |  | 0.0.. |  |  |
| `end_bar` | integer |  | 1.0.. |  |  |

### Result

```json
{
  "type": "object",
  "title": "punch",
  "properties": {
    "enabled": {
      "type": "boolean"
    },
    "start_seconds": {
      "type": "number"
    },
    "end_seconds": {
      "type": "number"
    }
  },
  "required": ["enabled", "start_seconds", "end_seconds"],
  "additionalProperties": false
}

```

### Errors

- `transport.range`: not one start and one end, or end not after start Hint: e.g. start_bar 5, end_bar 9

### Examples

**Bars 2-3 (120 BPM, 4/4)** (`empty`)

```json
{
  "start_bar": 2,
  "end_bar": 4
}

```

Result:

```json
{
  "enabled": true,
  "start_seconds": 2.0,
  "end_seconds": 6.0
}

```

## `transport.record@1`

Play and record every armed track's input.



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
    "recording": {
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
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["playing", "recording", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_armed`)

```json
{}

```

Result:

```json
{
  "playing": true,
  "recording": true,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>"
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
    "recording": {
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
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["playing", "recording", "seconds", "beat", "sample", "loop", "punch"],
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
  "recording": false,
  "seconds": 2.0,
  "beat": 4.0,
  "sample": 96000,
  "loop": "<any>",
  "punch": "<any>"
}

```

## `transport.stop@1`

Stop playback and return to where it started (ends a recording).



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
    "recording": {
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
    },
    "loop": {
      "type": "object",
      "title": "loop",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "punch": {
      "type": "object",
      "title": "punch",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "start_seconds": {
          "type": "number"
        },
        "end_seconds": {
          "type": "number"
        }
      },
      "required": ["enabled", "start_seconds", "end_seconds"],
      "additionalProperties": false
    },
    "recorded": {
      "type": "array",
      "items": {
        "type": "object",
        "title": "recorded_take",
        "properties": {
          "track": {
            "type": "string"
          },
          "item": {
            "type": "string"
          },
          "take": {
            "type": "string"
          },
          "file": {
            "type": "string"
          },
          "seconds": {
            "type": "number"
          },
          "lost_frames": {
            "type": "integer"
          },
          "takes": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        },
        "required": ["track", "item", "take", "file", "seconds", "lost_frames", "takes"],
        "additionalProperties": false
      }
    }
  },
  "required": ["playing", "recording", "seconds", "beat", "sample", "loop", "punch"],
  "additionalProperties": false
}

```

### Errors

- `engine.stopped`: the engine is not running Hint: engine.start first
- `record.nothing_armed`: no armed track has an input Hint: track.set {armed: true, record_input: [1, 2]}
- `record.busy`: the audio callback did not return for 2 s (still recording) Hint: stop again, or engine.stop (the files stay recoverable)

### Examples

**While the engine runs** (`engine_running`)

```json
{}

```

Result:

```json
{
  "playing": false,
  "recording": false,
  "seconds": "<any>",
  "beat": "<any>",
  "sample": "<any>",
  "loop": "<any>",
  "punch": "<any>"
}

```
