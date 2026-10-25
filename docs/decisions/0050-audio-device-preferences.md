# ADR-0050: Audio device preferences and per-client graph requests

**Status:** Accepted, 2026-10-25. Amends ADR-0004 (the 2026-09-26
implementation amendment's "`PIPEWIRE_QUANTUM` is not used") and ADR-0038 (the
GUI's native default backend). Headless defaults are unchanged.

## Context

The GUI started JACK on Linux and the device-free dummy everywhere else, so on
macOS and Windows nothing was heard until an agent or the actions list ran
`engine.start {backend: portaudio}`. Nothing chose devices, sample rate or
buffer size from the menus, and nothing remembered a choice. On Linux the
block was already a per-client PipeWire latency request, but a rate could not
be asked for at all.

pipewire-jack's documented client variables (`pipewire-jack.conf(5)`,
`pipewire-props(7)`) offer three strengths of request:

- `PIPEWIRE_LATENCY=<block>/<rate>` (`node.latency`): the client runs with
  this quantum or smaller; the graph takes the smallest request.
- `PIPEWIRE_RATE=1/<rate>` (`node.rate`): a suggestion, applied only while
  the devices are idle and the rate is in the server's allowed rates.
- `node.force-quantum` / `node.force-rate` (through `PIPEWIRE_PROPS`):
  forced on the whole graph while the node is active; the last node to
  activate wins, and the previous state returns when it stops.

A JACK client always runs at the graph's rate: PipeWire cannot resample one
client while the rest of the graph keeps its rate. Running Quire alone at
another rate would need a native PipeWire stream with an adapter (ADR-0004's
step 3), which adds resampling latency. The engine already plays any project
at the device's rate (media is resampled; renders stay at the project's rate).

`pw-metadata -n settings 0 clock.force-rate` would always work, but it is a
server-wide setting that outlives the process: system configuration, which
the hard rules exclude.

## Decision

1. **Preferences own the device.** `preferences.json` (supple.preferences/1.5)
   gains an `audio` object: `backend` (`jack`, `portaudio`, `dummy`; absent:
   the platform default), `host_api`, `output_device`, `input_device` (exact
   names; absent: the host API's defaults), `record` (open the input device),
   `rate` (0: the device's or graph's own), `block` (16..4096) and
   `force_graph`. Older files load with the defaults. Edit > Audio device…
   (and a Preferences row) edit it; each change is saved and a running engine
   restarts with it at once. At app start the saved choice is started; if it
   is refused, the defaults of the same backend are tried and the saved
   choice is kept for when the device returns; a failed start or that
   fallback opens the dialog, whose status says why. `--audio`
   still picks a backend with its defaults; offscreen and finite runs still
   start nothing.
2. **The GUI's default backend is the device**: JACK on Linux (unchanged),
   Core Audio's default devices on macOS, and WASAPI's on Windows. PortAudio
   lists ASIO first and would default to it whenever any ASIO driver is
   installed; ASIO holds its driver and fails while another program uses it,
   so it is a choice, not the default. Headless
   `engine.start` keeps dummy as its native default (ADR-0038), so CLI runs,
   tests and agents never open hardware unasked.
3. **Linux rate and buffer are requests unless forced.** An explicit
   `engine.start` rate with JACK sets `PIPEWIRE_RATE`; the new
   `force_graph` argument sets `PIPEWIRE_PROPS` to `{ node.force-quantum=<block>
   node.force-rate=<rate> }` (the rate only when given). Both are set for the
   `jack_client_open` call only, and a variable the user set wins, as
   `PIPEWIRE_LATENCY` already did. Forcing lasts exactly as long as the engine
   runs. The granted rate and period are reported, never assumed; plain JACK
   servers ignore the variables.
4. **Native rates.** An explicit rate is asked of the device: on CoreAudio,
   PortAudio switches the device's nominal rate
   (`paMacCoreChangeDeviceParameters`) and converts what the device cannot
   run. WASAPI shared mode always requests Windows' conversion
   (`paWinWasapiAutoConvert`), so a project rate other than the mix format's
   opens instead of failing. ASIO drivers set their own rate. The GUI's
   "own rate" passes the output device's default rate, so nothing converts.
   The buffer size is PortAudio's frames per buffer: CoreAudio's I/O size,
   ASIO's buffer (rounded by the driver), and a chunk size within WASAPI's
   shared-mode period.
5. **Inputs.** The native input device opens all its channels, up to 32 (as
   JACK does); ASIO uses the output device for both directions. Playback only
   opens none.

## Consequences

macOS and Windows users hear audio by default and can choose devices without
an agent. A forced graph rate or quantum changes what every other PipeWire
client gets while the engine runs; the dialog says so. The host-specific
settings pass through an optional `PortAudioApi::streamInfo` hook, null in
the fake driver, so the selection and lifecycle tests are unchanged and a new
test checks that the hook's settings reach both stream directions.

## Validation and limits

Fake libjack tests check each variable reaching the client, the user's
variables winning and the environment restored; an app test checks
`engine.start {rate, block, force_graph}`. GUI tests cover the canonical
round trip, every rejected field, the `engine.start` arguments for each
backend and host API, and the dialog's save/restart/fallback. Whether a
particular PipeWire server grants a suggested rate, and how CoreAudio, WASAPI
and ASIO devices behave at each rate and buffer, need device trials on each
platform; the fake tests do not establish them.

## Alternatives

A native PipeWire stream per ADR-0004 step 3 would let Quire alone run at
another rate, at the cost of a second Linux backend and resampling latency;
revisit if forcing proves too disruptive. Server metadata
(`clock.force-rate`/`clock.force-quantum`) was rejected as system
configuration. Separate rate and buffer force toggles were judged not worth
the extra row: forcing exists to make both hold for a session.
