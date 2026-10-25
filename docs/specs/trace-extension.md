# Spec: CLAP extension `com.suppleaudio.trace/1`

**Status:** Implemented. Header: `plugins/common/include/supple/clap/trace.h`,
with a byte-identical copy under `plugins/suite/semantic/include/supple/clap/`
for source exports. Any plugin may implement it; it is a debugging aid, not
part of a patch, a saved project or the plugin's parameters.

## 1. Purpose

A plugin's own code can say what a value was while it ran: a DSP script's
`trace("name", value)`. A host that traces the plugin gets each name's values
over time, binned, so whoever is debugging it (a developer, or an agent that
cannot attach a debugger) can see an envelope settle, a filter state blow
up, or a detector change its mind, without dressing up meters as probes.

## 2. Interface

```c
static const char CLAP_EXT_SUPPLE_TRACE[] = "com.suppleaudio.trace/1";
typedef struct clap_plugin_supple_trace {
    uint32_t struct_size;
    bool(CLAP_ABI* start)(const clap_plugin_t* plugin, uint32_t interval, uint32_t bins);
    bool(CLAP_ABI* read)(const clap_plugin_t* plugin, const clap_ostream_t* out);
} clap_plugin_supple_trace_t;
```

Both are main-thread and work only while the plugin is **inactive**, so the
audio thread's writes and the host's reads never overlap and need no lock.

- `start(interval, bins)` replaces any trace with a new one: from the first
  block processed after the call, each name the plugin traces gets `bins`
  bins of `interval` frames. `bins` 0 stops tracing. It returns false,
  changing nothing, while active, for an `interval` of 0, or for more bins
  than the plugin keeps (at least 4096).
- `read(out)` writes the document below. It returns false, writing nothing,
  while active or when no trace was started.

The host requires `struct_size` through `read` and non-null callbacks.
Future compatible members append to the struct; incompatible changes use
`/2`. Hosts that bridge plugins out of process do not offer traces.

## 3. Document

```json
{
  "schema": "supple.trace/1.0",
  "interval": 480,
  "bins": 100,
  "traces": [
    {"name": "envelope", "last": [0.12, 0.15, null],
     "low": [0.0, 0.14, null], "high": [0.12, 0.15, null], "non_finite": 0}
  ],
  "dropped": 0
}
```

Bin `b` covers frames `b * interval` to `(b + 1) * interval - 1`, counted
from the first frame processed after `start`. For each name, in the order
first traced: the last value given in each bin, the least and the greatest,
`null` in bins where none was given; `non_finite` counts NaN and infinite
values, which are not kept. `dropped` counts values given under names past
the plugin's limit (16 for scripts). Values given past the last bin are not
kept.

## 4. Script implementation

`Plugin::trace(name, value)` records at the block's first frame and
`trace(name, value, i)` at its frame `i` (script.h). The compiler takes a
name only as a string literal of 1 to 47 characters, and only in
`prepare()`, `update()`, `process()` and the voice hooks, and at most 16
distinct names in a script (`script.trace.names`); editor code cannot
trace. The suite's `ScriptLayer` keeps a `TraceLog` (script_runtime.h),
allocated by `start` and handed to the processor through an atomic pointer;
the log counts the frames processed since it was handed over (the
processor adds each block's), so neither `reset()` nor a redefinition
moves its bins, and a `prepare()` trace lands in the first. On the audio
thread `trace()` finds or adds the name (copied into the log and compared
by its text, never by where the text lies: a reloaded script may put
another name at the same address), bins the value and writes it: no
allocation, no lock. With no trace started it is one atomic load. A
redefinition hands the trace to the new definition's processor. The source export path compiles the same
`Plugin::trace`, so a stock plugin offers traces too.

## 5. Host use

`host::Instance` offers `traces()`, `startTrace(interval, bins)` and
`readTrace()`; a bridged (isolated) instance offers no traces. `dsp.test`
(sandbox and engine), whose instances are always in process, starts a
trace on each signal's fresh instance before rendering, with the signal in
`trace_points` bins (100 by default), deactivates the instance once the
render ends, reads the trace and returns it per signal as `traces`
(`name`, `last`, `low`, `high`, `non_finite`, numbers to six significant
digits) with `trace_interval_s`; NaN given to a trace and dropped names
become `problems`.
