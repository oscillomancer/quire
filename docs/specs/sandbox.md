# Spec: CLAP host library and the headless plugin sandbox (Phase 2)

**Status:** Frozen for Phase 2 implementation (2026-09-26). Changes via ADR.
**Libraries/binaries:** `host/clap-host` (`supple::host`), `host/sandbox`
(`supple::sandbox` library + `<slug>-sandbox` CLI with `--mcp` stdio mode).
**Depends on:** core-*, midi, notation, audio-file, command-bus,
control-protocol (optional socket mode), mcp.

The sandbox proves the core idea early: an LLM, over MCP, loads a plugin,
programs it in physical units and text patches, renders phrases written in
the notation, reads analysis reports, and iterates. It is also the plugin
team's test harness and the seed of the engine (the engine reuses
`clap-host` unchanged).

## 1. `host/clap-host`

```cpp
namespace supple::host {

struct PluginInfo {
    std::string id, name, vendor, version, description, path;
    std::vector<std::string> features;   // CLAP feature strings
    bool instrument;                     // declares note input and no main audio input
    bool semantic;                       // implements com.suppleaudio.semantic-params/1
};

Result<std::vector<PluginInfo>> scan(std::span<const std::filesystem::path> dirsOrFiles);   // supervised third-party probes; ADR-0013
std::vector<std::filesystem::path> defaultSearchPath();   // $CLAP_PATH, ~/.clap, /usr/lib/clap, /usr/local/lib/clap, + build dirs passed by the app

class Instance {   // owns one plugin instance; main-thread object
  public:
    static Result<std::unique_ptr<Instance>> create(const PluginInfo&, const HostIdentity&);
    const PluginInfo& info() const;

    // Lifecycle (main thread)
    Status activate(double sampleRate, std::uint32_t minFrames, std::uint32_t maxFrames);
    void deactivate();
    std::uint32_t latency() const;     // samples, valid when active
    std::uint32_t tail() const;        // samples; UINT32_MAX = infinite (capped by the renderer)

    // Ports (queried after init; selects a stereo config if offered)
    int audioInputChannels() const;    // main input, 0 for instruments
    int audioOutputChannels() const;   // main output
    bool acceptsNotes() const;

    // Parameters: standard CLAP view
    std::vector<ParamInfo> params() const;             // id, name, module, min, max, default, flags
    Result<double> paramValue(std::uint32_t id) const; // CLAP value
    Result<std::string> paramText(std::uint32_t id, double clapValue) const;
    Result<double> paramFromText(std::uint32_t id, std::string_view text) const;
    // Queue a CLAP value change; applied at the start of the next processed block (or via params.flush when idle).
    Status setParam(std::uint32_t id, double clapValue);

    // Semantic view (only when info().semantic)
    Result<json::Value> semanticSchema();
    Result<std::string> getPatch(bool changedOnly);
    Result<void> setPatch(std::string_view json, int mode);   // errors: the plugin's error array as Error details
    Result<std::string> describe(bool changedOnly);

    // State
    Result<std::vector<std::uint8_t>> saveState();
    Status loadState(std::span<const std::uint8_t>);

    // Processing (audio thread; RT-safe; SUPPLE_RT)
    void startProcessing(); void stopProcessing();
    bool process(const ProcessBlock&);   // see below
};
}
```

`ProcessBlock` carries planar float input/output pointers (in-place allowed
only if the plugin declares it), frame count, a sorted input event list
(note on/off with CLAP note ids, note expressions reserved, param value
events, MIDI 1.0 events for plugins without CLAP note ports' CLAP dialect),
an output event sink (param gestures/values the plugin emits), transport
(tempo, beat position, bar start, time signature, playing flag), and a
steady-time sample counter.

The host implements: `clap.log` (to the host log, never on the audio
thread synchronously: audio-thread log calls go to a lock-free ring),
`clap.params` (rescan/clear/request_flush), `clap.state` (mark_dirty),
`clap.latency` (changed), `clap.tail`, `clap.thread-check`,
`clap.audio-ports` (is_rescan_flag_supported), `clap.note-ports`,
`clap.timer-support` (main-thread timers, driven by the app loop),
`clap.thread-pool` (not provided), and `com.suppleaudio.semantic-params/1`
host side (records `changed` flags). `request_restart`/`request_process`/
`request_callback` set flags the app services on the main thread.

Thread discipline follows CLAP: create/activate/params-info/state/GUI on the
main thread; `process` on one audio thread. The sandbox's audio thread is
simply the render thread (offline), but the RT rules still hold and the
render loop runs under rtsan in the `rtsan` preset.

## 2. The sandbox model

A sandbox session holds:

- **instances** `in_1`, `in_2`, … (loaded plugins with their CLAP state);
- a **chain**: an ordered list of instance ids, the first may be an
  instrument; audio flows through the chain in order;
- **renders** `rd_1`, … (WAV files under the session work dir plus their
  analysis reports, kept for comparison);
- **settings**: sample rate (default 48000), block size (default 256),
  tempo (default 120 BPM), meter (default 4/4), render tail limit (default
  5 s), output format (float32 WAV).

Work dir: `$XDG_CACHE_HOME/<slug>/sandbox/<session-id>/` (or `--workdir`).
The command log is `commands.ndjson` in it.

## 3. Rendering

`render` builds the input (notation → `notation::compile`, or a MIDI file,
or an audio file resampled to the session rate with the audio-file
resampler), then runs the chain offline:

- activate all chain instances at the session rate with `[1, blockSize]`;
- set the transport (tempo, meter; playing = true; bar/beat positions
  advanced per block);
- process fixed-size blocks (the last block may be short); note events and
  param events land at their exact sample offsets within blocks;
- after the input ends, continue for the tail: until the chain's reported
  tail elapses or output stays below −120 dBFS for 200 ms, capped at the
  tail limit;
- compensate total chain latency by discarding that many leading samples;
- write float32 WAV; analyze if requested.

**Determinism:** two renders of the same session state and arguments are
bit-identical (test: render twice, compare bytes). Block-size invariance for
stock plugins is tested separately (render at block 1, 64, 256, 1024; null
within −120 dBFS for the stock test synth).

## 4. Commands (all `McpTool`)

| command | kind | args (abridged) | result (abridged) |
|---|---|---|---|
| `plugin.list` | query | `filter?` (text), `rescan?` | `plugins: [{id, name, instrument, semantic, features}]` |
| `plugin.load` | edit | `plugin` (id or path), `position?` (chain index, default: instrument first else end) | `instance`, `name`, `semantic`, `chain` |
| `plugin.remove` | edit | `instance` | `chain` |
| `plugin.describe` | query | `instance`, `detail` (`summary`/`full`) | `params` (key/unit/range/current, summary: grouped compact list; `active: false` while the parameter's module is not in the patch), `patch_type`, `patch_schema` (full only), `description` (prose), `docs` |
| `param.get` | query | `instance`, `keys?` | `values: {key: {value, text}}` |
| `param.set` | edit | `instance`, `values: {key: number \| "unit string" \| enum key \| bool}` (third-party: CLAP id or name, `{"text": "…"}` allowed) | `values` as applied with text |
| `patch.get` | query | `instance`, `detail` (`changed`/`full`) | `patch` |
| `patch.set` | edit | `instance`, `patch` (complete) | `description` |
| `patch.merge` | edit | `instance`, `patch` (RFC 7396 merge patch) | `changed` (the effective diff), `description` |
| `preset.save` | action | `instance`, `name`, `tags?`, `notes?` | `path` |
| `preset.load` | edit | `instance`, `name` | `description` |
| `preset.list` | query | `plugin?` | `presets: [{plugin, name, tags}]` |
| `render` | action | one of `notation`, `midi_file`, `audio_file`; `bars?`/`seconds?`; `tempo?`; `tail_s?`; `name?`; `analyze?` (`none`/`summary`/`standard`/`full`) | `render` (id), `path`, `seconds`, `peak_dbfs`, `analysis?` |
| `analyze` | query | `render` or `file`, `detail`, `note_off_s?` | `report` |
| `compare` | query | `reference` (render/file), `test` (render/file), `reference_note_off_s?`, `test_note_off_s?` | `deltas` (notable first), `ltas_rms_db`, `summary` (prose), `loss` (the optimizer's score: `total`, `terms`, `note_off_s`) |
| `masking` | query | `masker`, `maskee` (render/file) | masking report |
| `profile.evaluate` | query | `render`, `profile` (inline document) | per-target results, `loss` |
| `session.get` | query | — | settings, chain, instances, renders |
| `session.set` | edit | `sample_rate?`, `block_size?`, `tempo?`, `meter?`, `tail_limit_s?` | settings |
| `session.reset` | action | — | — |

Plus `batch` and `registry.*` from the bus. Every command has at least one
example running against fixtures (`empty`, `synth_loaded` with the probe
synth, `synth_and_render`).

Transactions: the sandbox implements `beginTransaction` by snapshotting the
state of affected instances (CLAP state + chain) and restores them on
rollback. Begin fails with `sandbox.snapshot_failed` at `/instances/<id>`
when any state cannot be saved; its `details` retain the plug-in error.
Rollback returns `sandbox.rollback_failed` with `sandbox.restore_failed`
details for each instance and nested plug-in errors. The command bus retains
the initiating command/commit error as a final detail when rollback fails.
Renders created inside a failed batch are deleted. Invocation savepoints are
independent of that outer snapshot; a failed step restores its plug-in states,
chain, settings, render set and pending Lua/host events while the transaction
remains open. The Result-returning invocation hook refuses the command when
capture fails. Restore failure is attached to the command error and aborts the
outer transaction even if Lua catches the call error. Snapshot/restore failures
also emit `sandbox.savepoint_failed` with the same structured error fields.

Preset documents accept `supple.preset/1.0` (optional numeric `version: 1`).
An absent schema is legacy; unknown/newer schemas or explicit versions fail
with `sandbox.preset_schema` or `sandbox.preset_version` before state changes.
Semantic schema entries require a numeric id, name, recognized type and the
range/choices for that type. Invalid schemas warn and fall back to plain CLAP
parameters. State loads invalidate queued pre-restore edits; inactive saves
flush acknowledged pending edits before writing plug-in state.

## 5. CLI

```
<slug>-sandbox --mcp [--workdir DIR] [--plugin-path P]...   # MCP over stdio
<slug>-sandbox --socket [PATH]                               # control protocol server
<slug>-sandbox exec '<json invocation or batch>'             # one-shot, prints result JSON
<slug>-sandbox registry [--markdown | --mcp-tools]           # generator output (used by drift tests)
```

## 6. Test instrument (P2.3)

`plugins/probe-synth` — an internal test instrument instantiated from the
template per ADR-0011 (not a stock product): one oscillator (saw/square/
sine/noise), a 12/24 dB low-pass SVF with resonance and envelope amount, an
ADSR amp envelope, a filter envelope, 8-voice polyphony, output gain; ~15
semantic parameters; flat patch type `supple.probe-synth-patch/1.0`;
semantic-params extension; deterministic and block-size invariant. It
exists to exercise the template instantiation, the extension, the sandbox and
the MCP loop before the flagship synth.

## 7. Acceptance (Phase 2)

A scripted end-to-end test drives the sandbox over MCP stdio: list plugins
→ load the probe synth → describe → set params with unit strings → merge a
patch → render a notation phrase with summary analysis → change the patch →
render again → compare → save preset. Its transcript is committed to
`docs/demos/phase2-mcp-loop.md`. A second test runs the same flow with an
invalid patch and an invalid notation string and asserts the errors carry
paths, locations and hints.

## Implementation notes — `host/clap-host` (2026-09-26, orchestrator)

- Output events go to a caller-preallocated `OutputEvents` sink (fixed
  storage + count + dropped), never a growing container, so `process()` is
  allocation-free; it is `SUPPLE_RT` and exercised under rtsan.
  `OutputEvent::flags` preserves CLAP event flags, including `DontRecord`
  (do not record automation) and `IsLive`. Note-end events retain their input
  note address `(port, channel, key, noteId)`; -1 is an unspecified/wildcard
  component, so a note id of -1 can be matched using the other components.
  Parameter values also retain their target address. VST3 output parameter
  values are normalized, with zero flags and address components of -1.
  Native VST3 note-off outputs are not synthesized as CLAP note-end events.
- `takeIdleOutputs()` drains CLAP `params.flush` output gestures/values on the
  main thread, local or bridged, including active-but-stopped audio-thread
  flushes. The instance retains a FIFO of 1024 events independently of the
  process sink; overflow drops newest and the returned `dropped` count is
  cleared on drain. Returned events have time 0 and the same flags/address
  form as process outputs. A drain is bounded by the queue's captured size;
  concurrent flush outputs not in that capture wait for the next drain.
  No allocation or lock occurs in the flush output callback. The returned
  vector allocates only on main. This is a CLAP flush channel; the VST3
  backing does not expose CLAP idle outputs. A failed bridge returns an empty
  drain and remains observable through `failed()`.
- `setParam()` flushes immediately through `params.flush` while the plugin is
  inactive (main thread); while active it queues into an SPSC ring delivered
  at time 0 of the next `process()`, or by `flushParams()` on the audio thread
  (CLAP requires the audio thread for flush while active).
- `serviceMainThread()` answers `request_callback`, performs `request_restart`
  (deactivate + activate with the same rate and max block) when not
  processing, handles `request_flush` while inactive, and fires due timers.
  `takeNotifications()` reports state-dirty, params-rescan, latency-changed
  and semantic-changed flags.
- Plugin log calls on the audio thread go to a lock-free ring (copied by
  hand, no formatting calls); other threads use a mutex-protected list;
  `takeLog()` drains both on the main thread.
- `scan()` loads each binary once (a path-keyed library cache shared with
  instances keeps each `.clap` loaded exactly while needed) and instantiates
  each plugin briefly to detect the semantic extension.
- Instances select a stereo `audio-ports-config` when offered; notes use the
  CLAP dialect unless the plugin only supports MIDI.
- The semantic-params ABI header lives at
  `plugins/common/include/supple/clap/semantic_params.h` (exported as the
  CMake target `supple::semantic-params-abi`).
- Hermetic tests use in-tree fixture plugins (`host/clap-host/tests/
  fixture_plugins.cpp`): a gain effect with the semantic extension,
  restartable latency and JSON state, and a sine instrument. The host was also
  checked by hand against the template's `Starter.clap` (scan, text I/O,
  flush, processing, state).

## Implementation notes — review round 2 (2026-09-26, orchestrator)

- **Lifecycle is enforced:** `startProcessing()` returns false when the plugin
  refuses (then `process()` returns false); `deactivate()` while processing is
  ignored and logged; destroying a processing instance stops it first (caller
  bug, recovered). `request_restart` raised while processing stays pending
  until processing stops (`restartPending()` says so, also for a bridged
  instance and a VST3 `kLatencyChanged`, which VST3 answers with a
  deactivate/activate; one reported during activation is only a notification); `request_flush` while active is served by the next
  `process()`/`flushParams()` on the audio thread (`needsAudioFlush()`).
- **Event arena:** queued `setParam()` changes only take the slots left after
  the block's own events; the rest stay queued for the next block. Block
  events beyond the arena are dropped and counted
  (`Notifications::droppedEvents`). MIDI-only note ports get a choke as a
  note-off.
- **Tail policy:** when every chain stage implements `clap.tail` with a finite
  value, the render stops exactly at musical end + latency + the sum of the
  reported tails (or earlier on the silence hold); an unreported or infinite
  tail leaves it to silence detection and the cap. `Notifications::tailChanged`
  reports `clap.tail.changed`.
- **Render limits:** at most 2^25 frames per render (~11.6 min at 48 kHz),
  checked before allocating; negative or out-of-range event times, non-finite
  or out-of-range tail/silence settings are errors (`render.too_long`,
  `render.event_time`, `render.tail`, `render.silence`). An explicit
  `lengthSamples` is authoritative; events past it land in the tail.
- **Session** (`session.h`): the model behind the §4 commands, independent of
  the bus. Third-party plugins' params are addressed by CLAP id or name with
  CLAP values or `{"text": …}`; semantic plugins' params go through
  `set_patch` merges so the plugin validates units and ranges (error paths are
  rebased from `/params/…` to `/values/…`). Presets live in
  `<presetDir>/<plugin id>/<name>.json`. IDs are never reused, even after a
  rollback.


### Catalogue diagnostics (2026-10-04)

`plugin.list` always returns `scan` beside `plugins`, including when no plug-ins
are available or a filter matches none. `scan.failures` contains `{module,
reason: {code, message, path, hint, details?}, cached}`; `scan.warnings` carries
structured errors and `scan.cached_modules` counts reused module results.
The report belongs to this catalogue and survives unrelated host scans;
`rescan: true` replaces both the catalogue and its report.
