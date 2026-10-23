# Status

Updated 2026-10-16. This file describes the present: what works, what is
known not to, and what is planned. Keep it current with each change; the
history is in git.

Quire 0.1.0 runs on Linux (X11 or XWayland, JACK through PipeWire or a JACK
server) and is used for real sessions. Full local CI (`tools/ci/run-all.sh`:
license, format, dev, release, gcc, asan, ubsan, tsan, rtsan) passes on
`main`.

The port branch has on-demand Linux/macOS/Windows CI and portable presets
(ADR-0034). Linux dev passes all 144 tests including the 12 CLAP validators.
Native headless services, hosting/scanning, bridge, stock wrappers, device
backend builds and GUI tests pass Windows CI (102 tests) and macOS arm64
CI (ADR-0036/0037/0038/0039).
Windows executables run with the UTF-8 process code page, so non-ASCII
paths work whatever the system locale; the minimum is Windows 10 1903
(ADR-0044). Not yet confirmed by Windows CI.
The macOS preset uses AppleClang and system libc++; shared float/double parsing
uses vendored fast_float (MIT) on all platforms, including the suite's
self-contained source exports. Native waits/wakes, thread setup, control
protocol and standalone MCP/proxy have native coverage on both platforms
(ADR-0035).
Both native platforms enable and test audio-file I/O, recording recovery,
engine/CLI, dummy/offline rendering and sandbox. PortAudio builds CoreAudio on
macOS and WASAPI/ASIO on Windows; fake-device tests verify RT/offline equivalence
in both executors. Actual hardware trials remain opt-in and unmeasured. The
GUI gate is lifted (ADR-0039), with native editor embedding (ADR-0040);
the combined native DAW builds and hermetic tests pass CI on both platforms.
Portable libraries, the session model, stock DSP cores
and the script suite build independently of those services; see
CONTRIBUTING.md for the complete gate list. On Windows, a save blocked by
another program that holds the file open (no delete sharing) retries for about
a second, then reports which file and what to close.

The bridge port implements native macOS shared memory/address wakes and Windows
mapping/event handle transfer, notifications and process isolation (ADR-0037).
All platforms launch and clean up through the shared ChildProcess helper, with
explicit IPC inheritance and process-group/Job ownership. Linux retains its
memfd/futex transport and pidfd death watch. Bridge stderr retains plug-in
diagnostics; scanner stderr remains suppressed.
Bridge targets and portable fixture tests are enabled and pass native CI on
macOS and Windows. Engine fixtures cover shared/dedicated crash and deadline
failures, structured errors and restart ownership. Sandbox rendering verifies
exact isolated/local output through the existing host/render APIs;
plugin.load continues to load in process. Linux bridge, engine and sandbox
tests pass under RTSan and TSan, retaining the existing deadlines and
background-only processing contract.

Native host editor paths now select CLAP cocoa/win32 or VST3 NSView/HWND,
with typed full-width parent handles (ADR-0040). Windows bridge editors embed;
macOS bridge editors own floating NSWindows and service AppKit with IPC.
Hermetic editor tests cover partial-open cleanup, resizing and repeated teardown.
Native visual/focus/IME validation remains opt-in; combined DAW-side native
shell/editor integration tests pass CI. Cocoa bridge windows remember their place
per effect like other editors (their focus is not tracked for MIDI learn); they
use normal level, order above the DAW window without activating the
bridge, and are raised again when the DAW window regains focus. A display-scale
change (monitor move, system scale) is passed to open Linux/Windows editors, local
or bridged, CLAP or VST3, and resizes them; hermetic tests cover the rule, the
visual result is unchecked without a desktop with mixed scales. Windows child
keyboard behavior is plugin-dependent.

Evaluation harness fixtures now run on Windows as well as POSIX: executable
fixtures use Python, credential references can use Windows hard links, and
attempt cleanup uses native Jobs or POSIX process groups. Real sandbox/model
checks remain explicit opt-ins. Source-export player/factory/audio conformance
runs natively, including binary exports: Windows retains the module trailer;
macOS uses a bundle resource and ad-hoc signing. The lifecycle and thirteen
source/binary export conformance cases pass CI on Linux, macOS arm64 and Windows.

## What works

- **Licensing** (ADR-0033): DAW code is GPL-3.0-or-later; stock plugins
  and their shared libraries are MIT.

- **Session and history** (`specs/session-model.md`, ADR-0008): immutable
  versions with stable ids, per-branch undo, branches with object-level diff
  and merge by replay (field-level conflicts, id remapping), A/B renders of
  two branches, a command log with origins, a history budget that squashes
  the oldest steps. Project format `supple.project/2.1` with migration from
  1.x, autosave, crash recovery of recordings, a per-project UI sidecar.
- **Command bus** (`specs/command-bus.md`, ADR-0007): about 120 typed
  commands with JSON Schema, units, examples run as tests and structured
  errors; `batch` transactions; events; the JSON-RPC socket; the MCP adapter
  (in process or as a proxy to the running app); Lua scripts on the bus
  (ADR-0006). The skill's command reference is generated and drift-tested.
- **Engine** (`specs/engine.md`): deterministic offline renders with
  real-time output bit-identical to them; anticipative rendering for
  non-live tracks (ADR-0012) and a live region for monitored tracks
  (ADR-0014); a DAG worker pool with graph-bounded participation and idle
  parking (ADR-0023), a separate live callback fork-join pool (ADR-0024)
  with FIFO JACK helpers, a claim-based lifetime join that does not wait for
  unclaimed helpers, and an Automatic/limit audio-thread preference;
  an opt-in host-process memory-lock preference with effective status and
  actionable OS failures (bridges excluded; cleanup also drops plug-in locks);
  a separate default-off 20 µs CPU idle-latency request with close-based
  cleanup and permission diagnostics (real-device validation is user-only);
  audio threads named by role (`audio-render`, `audio-bg-N` and others);
  the renderer, background helpers and bridge audio threads at SCHED_FIFO 10
  below a real-time callback when permitted, with a warning and normal
  scheduling otherwise, reported per thread class in `engine.status`
  (ADR-0026);
  plugin delay compensation (with per-FX
  `pdc: ignore`); parallel FX groups; per-FX oversampling (2/4/8x); input
  FX on armed channels, monitored and recorded; plugins keep
  running while the transport is stopped; renderer deadline/event waits
  (ADR-0028); headless main-thread service driven by notifications and
  deadlines (ADR-0030), including MIDI controls, plug-in subscriptions,
  render-ahead output consumption, jobs, restart and autosave; smooth plan swaps (10 ms ramps);
  every node, plug-ins and bridged plug-ins included, runs with FTZ/DAZ in
  every executor, and owned audio threads prefault their stacks (ADR-0025).
- **Strips and routing** (ADR-0019 to ADR-0022): typed channel, buss and
  master strips with Console 9 summing (switchable, level-matched); routes
  with taps, channel maps, mono and polarity; sidechain keys (an unrouted
  key deactivates a plug-in's key port or bus, so it can key from its own
  input); up to 128
  channels per strip with plugin pins and multi-output instruments; strip
  polarity and mono; the routing matrix.
- **Recording**: armed inputs to crash-readable WAV, input monitoring (needs
  the arm), MIDI monitoring and recording with overdub, loop recording into
  takes, punch, count-in, metronome.
- **Editing**: items, takes, comping lanes, fades and crossfades with
  shapes, time stretch and pitch (Signalsmith Stretch), markers, regions,
  tempo and meter maps, the item clipboard, a piano roll with velocity and
  CC lanes, an event list and a notation panel.
- **Automation**: envelopes on strip, route and plugin parameters,
  automation items with pools, take envelopes, touch/latch/write recording;
  automation points selected (click, Ctrl+click, or the arrange marquee,
  which takes items and points on any track at once) and moved, deleted,
  copied, pasted and reshaped together, across lanes, from the lane's menu or
  the keyboard.
- **Rendering** (`specs/engine.md` §3): mix, stems, regions and the render
  matrix; file-name patterns; loudness or peak normalization with a
  true-peak limiter; WAV (16/24-bit with seeded dither, or float, RF64
  beyond 4 GiB) and FLAC; render jobs and a queue; per-file analysis.
- **Plugin hosting** (ADR-0013, ADR-0017, ADR-0018): CLAP and VST3,
  in-process or isolated in shared or dedicated bridge processes (a crash
  fails only that plugin), supervised scanning with a cache, editors
  embedded in process and from bridges, editor windows that remember their
  place per effect, plugin state saved with the project. Bridge main loops
  park until socket/descriptor/request activity or the nearest plug-in timer;
  CLAP/VST3 requests use core-base's audio-safe coalesced wake relay
  (ADR-0029), and timers run without an open editor; remote changes/death
  wake the app without editor or audio idle polling (ADR-0031).
- **GUI** (`specs/gui.md`): arrange view, track panel, mixer, FX chains and
  the plugin browser, routing matrix, MIDI editor, render dialog, history
  dock with branches, actions list with key bindings and macros, editable
  menus, toolbar and mouse modifiers, screensets, two palettes in dark and
  light, preferences, and tap tempo in the tempo form. The transport has a
  master playrate knob, context menu, key-bindable pitch steps and
  per-project display range (ADR-0045). Projects and renders default to
  `~/Documents/<slug>-media` (the render folder is choosable), each project
  in a folder of its own with its own media; imports are copied into the
  project by default, and Save as into another folder copies the media. FX
  chain presets and project templates (with a copy of their audio) are
  saved, loaded and deleted in the app. Each effect's row in the FX chain
  has a presets menu: the plugin's factory presets, read-only, and the
  user's own, saved and deleted there (`fx.preset.*`, the sandbox's preset
  format, kept per plugin in the configuration folder). Takes: show all takes in lanes,
  explode onto tracks or along the track, delete one, Ctrl-drag one out, and
  render an item through its FX (MIDI to audio) in place or as a new take.
  The FX chain selects several effects (Ctrl/Shift+click) to bypass, offline
  or remove together; a double click opens an effect's editor. The window
  waits on app/local plug-in/ModelLoop notifications and demand-driven
  deadlines; stopped transport and stationary meters have no refresh timer.
  Input/commands coalesce UI sidecar checks, and writer completion wakes main.
  TCP rows keep their full top line at a 24-unit minimum; older sidecars
  clamp on load. Alt+wheel anywhere over the arrange view, and Ctrl+wheel
  over the TCP, resize every track by the same step, with the row under the
  pointer anchored (editable in the mouse map); Ctrl+wheel over lanes
  scrolls. Proportional track height zoom is removed; legacy
  mouse bindings fall back to current defaults and saved absolute heights
  still load. Every wheel combination over the TCP, the arrangement's lanes
  and ruler, and the MIDI editor can be rebound in Edit > Mouse
  modifiers.
- **MIDI learn** (ADR-0027): any plug-in parameter (the FX window's MIDI
  button, or `fx.learn.set` for agents) and any action (Ctrl+L in the
  actions list) learns a controller by moving it: CC, note, pitch bend or
  program change, absolute, three relative encoder modes or toggle, soft
  takeover, and "only when selected / focused / visible" for parameters.
  Parameter bindings are saved with the project and chain presets; action
  bindings in `controls.json`. "(MIDI CC)" actions drive the selected tracks'
  volume and pan, the master's volume, the master playrate, a beat jog and
  the last touched plug-in parameter; the learn dialog warns when a knob is
  learned for an action that only runs per press.
- **Agents**: the MCP server for the running app or a headless engine, for
  any MCP client (Claude Code, Codex, Hermes, …); the bundled skill
  (`skill/`). There is no built-in agent panel (ADR-0043).
- **Analysis and sound design**: `core-analysis` (BS.1770/R128 loudness
  validated against EBU Tech 3341/3342, spectral, pitch, envelope, stereo,
  comparison, masking); the CMA-ES patch optimizer (`specs/optimizer.md`);
  the LLM evaluation harness (`tools/eval-harness`).
- **Stock plugins** (ADR-0011, ADR-0047): Marram, Yarrow, Sundew, Treeline,
  Gorse, Hickory, Sorrel, Teasel, Bracken, Ghyll, Lichen, Withy, Pollia
  and Umber, each passing clap-validator, a reset determinism check, an unload
  test and the headless REAPER check;
  JSON patches and the semantic-parameters extension.
  Yarrow, the EQ, is the first written as a DSP script
  (`plugins/yarrow`) and compiled natively from it, playing the same
  samples as the script in the player: eight matched zero-latency bands
  (bells, shelves, cuts to 48 dB/oct, notch, tilt) on stereo, mid, side or
  one channel, dynamic bells and shelves, band listening, gain scale, auto
  gain, and an editor of its own with a 32768-point spectrum line. It
  replaced Facet (`com.suppleaudio.eq`). Sundew, the compressor
  (`plugins/sundew`), is the second: six characters (Clean, Glue,
  Opto, FET, Vari-Mu and the upward Bloom), the feedback ones solved sample
  by sample; an external key, a key high-pass, link, mid/side, lookahead
  (a restart), tempo-locked release, colour, mix and loudness matching; an
  editor with a scrolling history with bars and beats, a transfer curve, a
  gain-change needle and a plot of each character's ballistics. It replaced
  Ballast (`com.suppleaudio.comp`). Treeline, the limiter
  (`plugins/treeline`), is the third: a true-peak ceiling (a 4×
  interpolating detector, within 0.01 dB to 20 kHz), attack along two
  overshoot-free curves blended by how sudden the music is, a hold that
  spans a cycle of a leading bass line, a two-stage release, 4.5 ms of
  latency at 48 kHz; BS.1770/R128 loudness of the output (momentary,
  short-term, integrated and LRA over the last 30 s or accumulated since
  play, true peak) against a target,
  level-matched and delta listening, and an editor with a history of what
  was held down, the loudness and the reduction in its sustained and peak
  parts, and plots of what the engine is doing. Gorse, the transient
  shaper (`plugins/gorse`), is the fourth: attack and sustain the
  same at any level, a detector whose hold spans a cycle of what plays (so
  held tones, bass lines and voices get no gain), a focus on the lows,
  mids or highs, a 1 ms lookahead that lands the gain on each onset, and a
  soft clip that keeps sample peaks under 0 dBFS; an editor that lays the
  recent hits over each other at their onsets and shows what each part
  changed, and a history with each hit it found. It replaced Flint
  (`com.suppleaudio.transient`). Hickory, the stereo utility
  (`plugins/hickory`), is the fifth: gain, mute, polarity, channel
  routing (swap, left, right, mono, side), width, balance, bass mono on
  the side only, a DC block and a delay of up to 50 ms to line tracks up,
  every switch crossfaded and the output the input bit for bit at rest;
  readouts of the last 3 s (peak, correlation, balance, side level) and an
  editor with an image of where the sound sits and the correlation an
  octave at a time, where bass mono's crossover is dragged. It replaced
  Plumb (`com.suppleaudio.utility`). Sorrel, the gate and expander
  (`plugins/sorrel`), is the sixth: with lookahead (a restart, 2 ms
  by default) its raised-cosine opening ends a millisecond before the
  level crosses the threshold, so onsets pass whole; it closes at a steady
  rate in dB, eased in, and with Follow at the rate the sound was already
  decaying at (within a factor of four of the release); hysteresis, hold,
  range to exact silence, Expand with a ratio, an external key with high-
  and low-pass, and key and delta listening. It keeps the key's levels of
  the last 10 s as a histogram and reads from it a noise floor, a loud
  level and a suggested threshold (the close level 2 dB above the noise's
  peaks), with the time open and the openings; its editor shows what came
  in, what goes out and what was taken, the histogram, the gain, a
  transfer plot with the hysteresis loop and a sketch of the timing. It
  replaced Sluice (`com.suppleaudio.gate`). Teasel, the saturation
  (`plugins/teasel`), is the seventh: twelve curves in three
  families (soft, tape, tube, transformer; transistor, diode, clip, fuzz;
  fold, rectify, crush, alias), four of them with memory (tape's
  hysteresis, tube's sag, a transformer core that saturates the bass
  first, a sample-and-hold), each with a character control and bias;
  driving only a band, the mid or the side; emphasis, cuts, tuned feedback
  through the curve, dynamics that drive the attacks or the sustain, and
  an LFO free or in time on the drive, character, bias or pitch;
  oversampling to 16x, minimum or linear phase, with the dry signal and
  the clean bands inside it,
  and auto gain that holds a −18 dBFS RMS signal's level at any drive.
  Its readouts give the THD and even share of a 100 Hz sine through the
  curve as set and the distortion it adds to the music; its editor shows
  the spectrum in and out with what the curve adds, the curve with where
  the music sits on it, the harmonics, the dynamics and the LFO. It
  replaced Kiln (`com.suppleaudio.drive`). Bracken, the delay
  (`plugins/bracken`), is the eighth: each side's loop in two halves,
  so every second echo can come early or late with each pair still on the
  beat (groove); stereo, ping-pong or mono; time free or in note values,
  bent like tape or faded to when it changes; tone, diffusion and a pitch
  shift in the loop; echoes forward or reversed; clean, tape or a bucket
  brigade (darker the longer the time) worn by age, with drive into the
  medium's curve; modulation, ducking, an input to throw sounds in by,
  width, and a freeze that holds the loop exactly. It tells the host its
  tail as the time, feedback and tempo change, and reads out the echo time
  and how long the echoes take to fall 60 dB; its editor shows the echoes
  to come (dragged across for the time, up and down for the feedback) with
  a playhead from the last sound, each echo's tone, the medium and the
  ducking. It replaced Echelon (`com.suppleaudio.delay`). Ghyll, the
  reverb (`plugins/ghyll`), is the ninth: a room built from its size
  in metres, the materials of its walls, floor and ceiling, and where the
  source and the listener stand. Its reflections are the room's mirror
  images off up to three surfaces, each as late, weak and dark as its path
  and heard through an ORTF or spaced pair, a head or one omni; its decay is
  each octave's Eyring reverberation time, the air's absorption counted;
  its tail is a sixteen-line network as long as the room's mean free path,
  at the reverberant level for the listener's distance; below the
  Schroeder frequency the room's modes ring at their own frequencies, as
  loud as the two places sit on them. Decay, scatter, predelay, early and
  late levels, cuts, width and a freeze. It reads out the RT60 at three
  octaves, the distance, the critical distance, the first reflection,
  clarity (C80) and definition (D50); its editor is the room to scale (the
  source, the listener and the walls dragged), a section for the height,
  the materials, every reflection on a time line (one hovered traces its
  path through the room), and each octave's decay with the modes beneath.
  It replaced Nave (`com.suppleaudio.reverb`). Lichen, the analyzer
  (`plugins/lichen`), is the tenth: it never writes its output (host-
  smoke holds it to a bit-exact pass-through) and reports as read-only
  parameters BS.1770 loudness (momentary, short-term, integrated, range,
  their highest), true and sample peak, PLR, crest, overs, clipped runs,
  DC, correlation, width, the width below 250 Hz, balance, mono loss, the
  seven named bands, tilt and centroid (as the offline analysis defines
  them) and the key with its strength, over the last 60 s of the play (or
  render), or all of it with `accumulate` (the editor's clock, as in
  Treeline). Agents read them with `fx.meters`. Its editor shows the loudness against
  a target, the last minute and the play's distribution, the spectrum now
  (mid and side), its peak and the play's average with the named bands, a
  vectorscope with correlation by third octave, the pitch classes round
  the circle of fifths, and a health list. It replaced Gauge
  (`com.suppleaudio.analyzer`).
  Marram, the synthesizer (`plugins/marram`, `docs/specs/marram.md`),
  replaced Orrery (`com.suppleaudio.orrery`, removed); the synth guide
  (`skill/guides/sound-design.md`), the evaluation tasks, the optimizer's
  tests, the MCP demos and the B1 recipe use it. Four
  oscillators (analog with sync, wavetables from Orrery's recipes, FM
  operators, noise, sub; FM between any two), unison up to 8 copies, two
  filters (ladder, state-variable, comb, formant; serial or parallel),
  drive, amp and mod envelopes with fixed amounts (cutoff, pitch, FM depth),
  velocity and key tracking, drift, glide, Poly/Mono/Legato with a
  polyphony limit, oversampled 1x/2x/4x; modulation routes from a third
  envelope, three LFOs (per note or free, synced or not, seven shapes),
  four macros and the MIDI sources to some sixty per-voice destinations;
  and fifteen factory sounds. Its parameters say when they apply, so an
  agent sees only the oscillators and filters a patch uses. Its editor
  draws each oscillator's wave (the wavetables' own frames), the filters'
  response with a node to drag, the envelopes and LFOs with handles and
  the newest note's place, the drive curve, the voices playing and the
  route list. It passes the stock checks and the REAPER check.
  Withy, the chorus (`plugins/withy`), is a stock plugin with no
  predecessor. Five styles lay out its voices: classic (two in anti-phase,
  left and right), dimension (two cross-mixed: the widest, with the
  shallowest combs), ensemble (three a third of a cycle apart), multi (two
  to eight at their own delays, rates and places) and vibrato (one voice, no
  dry). The depth is the pitch each voice swings in cents, the same at any
  rate, free or in note values locked to the song; sine or rounded
  triangle, a fast shimmer, a slow random drift, and bloom, which holds the
  voices still at each new note and lets them sway again (a singer's late
  vibrato). Voices read through a 16-tap windowed sinc, flat within
  0.01 dB to 16 kHz however fast they sweep; clean, or a bucket brigade
  whose band follows its clock, with its lean and compander hiss. Low keep
  leaves the lows dry and centred; high cut, feedback with its level held,
  width and an equal-power mix. It reads out the detune, the sweep, the
  rate and the mono loss; its editor weaves each voice's delay over the
  last 3 s, shows each voice's pitch now, where the voices sit around the
  listener, and the response now in stereo and folded to mono.
  Pollia, the phaser (`plugins/pollia`), has no predecessor
  either: 1 to 24 trapezoidal all-pass stages, stacked or spread over
  up to six octaves, with feedback either way (its added level mostly
  taken back); a sweep by an LFO free or in time (six shapes, a phase, a
  stereo offset) or by the playing's envelope, measured against its own
  recent peak so it needs no threshold, either direct or through a
  photocell (a lamp the cell follows, quick up and slow down); and rise
  and fall, a barber pole that turns the wet's phase through a 90-degree
  pair so the notches climb or sink forever at full depth. Keep below
  leaves the lows whole; mix runs from dry through the deepest notches to
  a vibrato. It reads out the stages' centre and the envelope now. Its
  editor draws the response with its notches, where the notches went
  over about a cycle and a half of the motion, and the path a photocell
  takes the sweep on; dragging the response moves the centre (across)
  and the range (up and down). Twelve factory patches.
  Umber, the tape machine (`plugins/umber`), has no predecessor: a tape
  machine modelled part by part and fitted to published data for current
  studio tape (third harmonic about 0.1% at reference level, 3% at
  +12.5 dB at 15 ips; saturation of the highs; noise). The drive is
  calibrated as a machine is (0 VU, reference level, is −18 dBFS RMS).
  Record equalisation lifts the highs into the tape as the losses at short
  wavelengths and the standard (NAB or IEC; AES at 30 ips) require, so
  highs saturate first and sooner at slower speeds; playback undoes it
  exactly. The oxide's magnetization is a tanh of the field (single-domain
  needles) squared off by the tape's squareness, its irreversible part
  pinned and relaxing toward that curve by an exact exponential step, so
  bias decides how much hysteresis is left: none audible at the
  recommended bias, an opening loop and grain below it. The playback head
  adds its bump (about 3.8 Hz per ips), dip and aligned top; wear adds
  spacing and azimuth loss, a lean between the channels, magnetized heads'
  even harmonics and dropouts; the transport adds wow (reels, a drift)
  and flutter (capstan, idler, scrape) read at the oversampled rate; hiss
  and modulation noise come from the tape, calibrated A-weighted. Three
  tapes, four speeds; full latency (8 ms and linear-phase oversampling,
  for deep wow) or low (under 10 samples, for tracking). It reads out the
  VU level, the headroom to the tape's 3% limit and how much the peaks
  were rounded. Its editor shows the machine (reels turning at the tape's
  speed, heads, VU meters and a peak lamp), the oxide's flux against the
  field live over its curve (dragged for drive and bias) and the response
  quiet and hot. Twelve factory patches, Mastering among them for the
  subtle rounding of a mix's peaks.

- **DSP scripting** (ADR-0032): plugins written in a checked subset of
  C++, run by a JIT player plugin, exported as plugins or C++ projects. The
  compiler and runtime exist (`plugins/suite`, tested against the same scripts
  compiled as C++). So does the player (`plugins/suite/player`, built from
  `plugins/template`, staged with the stock plugins): one plugin per script
  in the script library, with the semantic-params extension, reloading a
  script when its file changes. `dsp.write`, `dsp.check`, `dsp.get` and
  `dsp.list` write and compile scripts (the app's library is
  `<data>/<slug>/scripts`, shared with the sandbox), `dsp.test` measures
  one with standard signals at any sample rate (and, when asked, parameter
  automation, a timed sidechain key or a note sequence) and reports its
  meters as of each signal's end; a script tells the host a tail that
  follows its settings and the tempo (`setTail`), and a script that compiles is loaded
  and set like a stock plugin (`skill/guides/dsp-workflow.md`).
  `fx.describe` and the sandbox's `plugin.describe` report a live
  instance's loop-budget stops, out-of-range indices and failed reloads as
  structured problems. Scripts declare read-only factory patches
  (`factoryPatches(Patches&)`), listed and applied with `fx.patch.list` and
  `fx.patch.set {name}` or the sandbox's `preset.list/load {source:
  "factory"}`, and offered by exports' preset bar and hosts' preset
  browsers. A parameter may apply only while a toggle, a choice or a number's
  range declared above allows it (`.activeWhen()`, `.activeUnless()`,
  `.activeAbove()`, `.activeBelow()`): the schema marks it
  `"active": false` and says when it applies, descriptions leave it out,
  `plugin.describe`, `patch.merge`, `fx.patch.merge` and `fx.param.set`
  name it under `inactive`, and the editor, its own or the automatic one,
  dims its control. Scripts declare modulation sources (`Source` members,
  per voice or global; instruments also get velocity, key, random,
  pressure, slide and five MIDI controllers), and the patch's `routes`
  member connects them to `.modulated()` and `.polyModulated()`
  parameters with amounts in the parameter's unit (a frequency's in
  octaves), through a second source when given; the schema lists the
  sources and each parameter's amount unit, errors name the right unit,
  state and presets keep the routes, and factory patches set them. An
  instrument's `setVoiceLimit()` plays fewer of its voices. The editor
  shows the routes as a list to edit (the automatic editor's, or
  `e.routes()` in a script's own), written through the patch's checks.
  `dsp.export` makes a script a CLAP plugin for any host (a copy
  of the player with the script appended, which offers that one plugin), or
  a C++ project of the plugin template that builds the same plugin, bit for
  bit, with the script compiled as C++ (every example's export is checked
  against the player, for semantic conformance and factory patches, and
  passes its own tests). Effects and instruments
  (voices the runtime manages, CLAP note expressions and per-note
  modulation; no note effects); samples and impulse responses
  as files a patch names (loaded whole, no streaming from disk); spectral
  effects (an STFT), oversampling and sidechains. A script may draw its
  own editor (`editor()` for controls, `draw()` on an HTML-canvas-style
  `Canvas`, `pointer()` to drag and click what it draws, editing
  parameters as a user would, `Shared` members for what the audio code
  shows it); one that does not gets the automatic editor. `dsp.preview`
  draws a plugin's editor to a PNG without a visible window (EGL on Linux,
  Metal on macOS, WGL on Windows; the editor-picture extension), after clicks, drags and wheel turns it is
  given, and says what they edited. Plugin-file exports run on Linux,
  macOS and Windows.

Benchmarks are in `docs/benchmarks/`: B1, 75 Marram tracks with EQ and
compression at 48 kHz and 64 frames without xruns using anticipation, 28 to 29
without (`b1.md`); the comparison with REAPER, which sustains 82 to 83
(`b2.md`); GUI cold start and frame costs (`gui.md`); Console 9
strip cost (`console9-strips.md`). The benchmark harness also supports 64/128-frame requests, installed third-party effects,
whole-callback/render-job tails and an observed ring low-water mark. Timing is
bench-only; synthetic deadline counts and server xruns are separate. Historical
capacity reports retain their original revision/settings.

## Known limits

Engine and hosting:
- The headless platform layer (ADR-0035) has natively tested macOS/Windows
  address waits, wake sources, control socket/MCP paths and user directory roots.
  Both native engines run with dummy/offline audio and native RT/offline
  equivalence coverage. Windows file helpers and audio-file I/O pass native CI.
  Native hosting/scanning (ADR-0036) and PortAudio device builds (ADR-0038) are
  verified; native bridge targets are enabled (ADR-0037), with native host
  editor paths supplied by ADR-0040. Hardware behavior still needs opt-in device trials.
  Windows event waits admit 64 unique handles. Generic macOS High QoS must be
  self-applied. Native device helpers instead use a startup barrier with Mach
  time constraints (macOS) or MMCSS Pro Audio (Windows). On macOS the live
  helpers also join the opened device's CoreAudio workgroup (engine.status
  `workgroup`, ADR-0038); the renderer and background pool and the out-of-process
  bridge do not (the bridge would need a Mach-port channel it lacks). Hardware
  trials of the join remain opt-in and unmeasured. Linux process memory-lock and CPU
  latency preferences report unsupported on other platforms. Windows MCP is
  intended for piped/file input; cancelling an interactive console read can
  await a keypress during shutdown.
- REAPER's default anticipative FX sustains about 10 % more tracks than
  Quire with anticipation on B1's session (82 to 83 against 75, B2). The
  anticipative renderer is the limit: it ran at 78 % of real time at 64
  tracks while the callback stayed under 5 %.
- Live multiprocessing requires at least two usable CPUs and successful
  helper scheduling; otherwise the engine reports a warning and runs serially.
  A serial plug-in chain cannot benefit, and a stalled helper that has claimed
  a job stalls its cycle. Wake/scheduling overhead remains on light graphs;
  there is no adaptive cost threshold.
- The shared DSP guard implements x86 FTZ/DAZ and arm64 FPCR FZ; macOS arm64 CI
  verifies FPCR restoration and subnormal handling. Plug-in lifecycle calls run
  in the thread's own floating-point environment.
- On Linux, real-time background threads need `RLIMIT_RTPRIO` ≥ 1, an unlimited
  `RLIMIT_RTTIME` and a pool that leaves two allowed CPUs free; without them
  the renderer and pool run at normal priority (with a warning) and compete
  with desktop load.
- A bridged plug-in's audio thread that cannot take the engine's background
  policy stays at normal priority with no warning or status: nothing reports
  it to the host. The bridge inherits the host's limits, so this needs a
  limit or policy change made to the bridge process from outside; such a
  thread's plug-in can then fall behind under load like an unelevated
  renderer.
- There is no pipelined bridge mode, so a bridged plugin on a live
  (monitored) track is passed through with a warning; set its isolation to
  in-process to hear it while monitoring.
- Audio backends: Linux JACK, dummy, and native PortAudio (CoreAudio on macOS;
  WASAPI/ASIO on Windows), with selection, latency/xruns and restart notifications.
  Fake-device output equals offline in both executors, with native CI coverage.
  Hardware latency, disconnect behavior and scheduling benefit are unmeasured.
  Refusing native MIDI inputs are skipped with structured startup warnings;
  audio and other inputs keep running. CoreMIDI/WinMM use bounded per-port queues,
  timestamp mapping, bounded per-port merging, routing/learn/recording and
  non-restart topology advisories
  (ADR-0042). Device-free timing/overflow/lifecycle tests cover the shared path;
  physical MIDI timing/disconnect behavior needs SUPPLE_MIDI_DEVICE_TEST=1 trials.
  SysEx is discarded; WinMM timestamps have millisecond resolution. WinMM names
  do not distinguish two identical devices, or a replacement from the original;
  this will be addressed with Windows MIDI Services once its App SDK is released
  for production (ADR-0042).
  No direct PipeWire/ALSA backend or MIDI outputs.
- MIDI learn has no OSC, no "arm envelope" option, and no learn directly on
  track faders (the "(MIDI CC)" actions cover the selected tracks). The last
  touched parameter misses edits in plug-in editors that report no gestures
  or outputs. Every MIDI input is heard as a control;
  there is no per-device "control only" setting.
- Plugins that change state without notifying the host (LSP, for one) have
  that state captured only when the project is saved: no undo step and no
  dirty mark.
- Stopped-transport audio from tracks that are not live (a plugin's own
  preview button, for example) is not heard in anticipative mode unless the
  track is armed and monitored.
- A plugin's factory presets are the semantic extension's factory patches:
  a third-party plugin's own presets (CLAP preset discovery, VST3 program
  lists) are not listed in the presets menu, and the menu does not mark the
  preset last loaded.
- Multi-output plugins: main output ports wider than stereo are not used,
  and there are no per-output meters.
- Overlapping gestures on a plugin without semantic parameters share one
  state blob.
- `fx.meters` reads a plugin's readouts live or as the last `render.mix`
  ended; other renders (stems, regions, A/B) keep none.
- FX containers are not implemented; parallel FX run in series on pinned
  stages and strips with auxiliary channels, and oversampling is not
  available to plug-ins with auxiliary outputs.
- A bridged plug-in's tail is read once when a sandbox render starts, so a
  tail it changes during that render is not followed (in-process plug-ins'
  are).

Session and editing:
- Master playrate 0.25..4x and its tr_0 envelope persist in schema 2.1.
  Tape and pitch-preserving source conversion run before take FX/live mixing;
  MIDI, automation, plugin tempo, loops and transport share the fractional clock
  (ADR-0045). The transport knob, menu and the master Playrate lane edit it;
  knob ranges are UI state and never clamp saved values. Seeded stretch phase
  variation uses a portable integer RNG, so it matches across standard
  libraries. Non-unity audio/MIDI
  recording is refused, including queued transitions. Writing retains coarse
  strip semantics. Compatible scalar-only live edits publish bounded clock
  controls, retain source/converter state and glide over the time between
  moves through a 150 ms low-pass (one smooth pitch glide for a mouse drag or
  MIDI knob, at the cost of some extra lag) without graph
  compilation or an epoch change (ADR-0046). Structural prediction retains
  pending scalar ramps. A knob glides through 1.0 too; leaving 1.0 from the
  unity bypass glides the same way. Mode/curve edits wait for compensated lookahead
  and coalesce during prepared transitions; long-latency take chains can exceed
  the 200 ms horizon.
  All source allocation/priming runs on the control thread; the renderer and
  callback adopt prepared state at scheduled boundaries under RTSan. Prepared
  boundaries and lateness checks use the renderer frontier, block granularity
  and observed preparation time. A live knob move becomes audible after about
  350–400 ms (the render horizon plus clock and source lookahead). Seeks prepare
  off the audio executors while playback/monitoring continues. Clock windows
  reserve four additional seconds for latency changes; history exhaustion or late
  preparation recovers automatically through a prepared locate at the consumed
  position, with the same state reset/preroll as a seek. Moving a compensated
  stretch source uses this recovery too, preserving phase alignment.
  Render-command budgets conservatively use the minimum curve value; a long
  varying-rate range may need splitting even when its exact duration would fit
  the output budget.
- Take `stretch_markers` are stored but not applied; stretching and
  resampling run synchronously when a plan is compiled.
- Track groups are preserved in project files but not implemented.
- Loop recording passes that span a change of the loop range are not
  handled; MIDI recordings have no crash recovery.
- `fx.copy` does not copy envelopes or key routes; take FX cannot be keyed.
- The history budget's size estimate walks shared payloads per version, and
  squashed steps keep large invocation arguments.

GUI:
- Native shell/toolkit builds and tests pass CI (ADR-0039).
  Cocoa/Windows hidden-window tests replace Linux EGL offscreen;
  Linux golden references are unchanged. Native editor embedding is integrated
  (ADR-0040), with combined native builds and hermetic tests passing CI.
  Windows config keys use a protected current-user ACL; filesystems without
  ACL support fail the save. macOS cold-start timing reads the kernel process
  start time. GUI socket/screenset and source-export integration fixtures are
  enabled and pass native CI on macOS and Windows. macOS bulk rectangle edges
  align to framebuffer pixels so partial repaints match complete repaints exactly.
  Script-player headless pictures (`dsp.preview`) use framework 1.24.0's
  EGL, Metal and WGL renderers. Native CI requires pictures and pointer edits;
  missing devices/drivers return `dsp.no_picture` with a corrective hint.
- No autoscroll during drags; no resize cursor; plugins can be dropped onto
  mixer strips but not onto track-panel rows.

Plugins:
- DSP source exports require a visible, automatable parameter with a plain
  name for their host tests and do not retain the JIT's loop budget (a loop
  that never ends hangs a native export). Exports include declared factory
  patches, but do not package external audio files or the patch of a
  currently loaded instance. Preview requires an editor build and an
  available offscreen renderer (EGL, Metal or WGL with framebuffer objects).
- Yarrow has no linear-phase mode; its editor (drawn by the script) takes
  no keyboard input or right-click menus, draws the summed curve without
  regard to each band's channel, and analyses the output only. Its
  analysis costs about 0.3 ms per transform in a standalone -O2 copy (at
  most 25 a second at 48 kHz, under 1% of a core, on the editor thread);
  the plugin itself has not been profiled.
- Lichen's readouts refresh every 100 ms and reach a host as meter events
  at 30 Hz, so a render's last reading can miss its final 100 ms or more.
- clang-cl builds the suite runtime and stock scripts with
  `/clang:-ffp-contract=off` and `/clang:-fwrapv` (they had neither); native
  Windows CI has not run since.
- Scripts need a JIT: there is no fallback for hosts that forbid executable
  memory (MIR's interpreter is a test backend, not real-time safe).
- A script save that redefines the plugin while an editor drag is under way
  can leave that gesture open in the host until it rescans.
- Stock CLAP builds and product/conformance/reset checks run on Linux, macOS
  and Windows. Existing macOS AUv2 targets compile in CI but are not host-tested
  here. Stock VST3 builds remain opt-in; native VST3 hosting is tested with an
  in-tree SDK fixture. The pinned clap-validator checks every stock plugin
  and the player on all three platforms in CI; it stays optional locally
  (`tools/validator/build.sh`), and its tests skip without it. On macOS,
  `mac-embedding` (template 1.31.3) opens each stock editor in a Cocoa host
  window, as `x11-embedding` and `win32-embedding` do on Linux and Windows.

Smaller recorded items: `describeFlat` skips hidden parameters in full
detail; core-params formats magnitudes beyond six decimals imprecisely;
`Rng::uniform` at extreme endpoints; forced-schedule tests for `RtHandoff`.

## Planned

- Profile the 64-frame worst case; a pipelined bridge mode.
- Close the capacity gap with REAPER (B2) by making the anticipative
  renderer cheaper, then keep refining capacity in both modes.
- Track groups, stretch markers, MIDI outputs, a background media cache.
- Lua ergonomics (a script library path, hot reload).
- Hardening and ports beyond Linux.
