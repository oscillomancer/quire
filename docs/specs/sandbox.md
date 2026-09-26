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

Result<std::vector<PluginInfo>> scan(std::span<const std::filesystem::path> dirsOrFiles);   // dlopen, read factory, dlclose
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
| `plugin.describe` | query | `instance`, `detail` (`summary`/`full`) | `params` (key/unit/range/current, summary: grouped compact list), `patch_type`, `patch_schema` (full only), `description` (prose), `docs` |
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
| `compare` | query | `reference` (render/file), `test` (render/file) | `deltas` (notable first), `ltas_rms_db`, `summary` (prose) |
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
rollback. Renders created inside a failed batch are deleted.

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
