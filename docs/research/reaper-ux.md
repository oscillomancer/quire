# REAPER 7.80 workflow research for Quire

Research date: 2026-09-26. This is a workflow and data-model catalogue, not a UI or asset specification. Evidence notation: **[verified: ...]** means observed in the named local file, REAPER 7.80 command output, an offline render, or the bundled *REAPER Quick Start* PDF; **[from knowledge]** means a behavior remembered from REAPER use/public API descriptions but not reproduced in this sandbox. A file observation verifies its serialized representation, not every UI behavior or the meaning of every undocumented integer. Do not infer undocumented enum values from this note. No network was used. No proprietary source, binary, theme, or asset was inspected.

Evidence files and exact calls are listed in [methods.md](reaper/methods.md). [generated.RPP](reaper/generated.RPP) is a REAPER-saved project made with a temporary Lua script (then edited only to make its audio path relative and clear the temporary render destination); [tone.wav](reaper/tone.wav) is an original one-second test tone. The project has eight tracks (one inherited from the seed project), two folders, three explicit sends, a four-channel destination, a two-take item, ReaEQ take FX, ReaComp track FX, MIDI, a tempo ramp, meter change, marker and region. `reaper -newinst -nosplash -cfgfile /tmp/reaper-ux-cfg/reaper.ini -renderproject /tmp/reaper-ux-seed.RPP` exited 0 and produced a 529,890-byte WAV. [hand-authored.RPP](reaper/hand-authored.RPP) is the minimal MIDI source and [normalized.RPP](reaper/normalized.RPP) is its REAPER resave. The minimal hand-authored MIDI project also rendered and was re-saved by REAPER; the resave added defaults, IDs, and chunks. The temporary configuration was copied from `~/.config/REAPER`; that directory was only read. The command-line `-renderproject` route exited without an audio-device session; the interactive save route wrote a file but had to be timed out, so GUI dialogs were not systematically inspected. The render log included `error creating client` from the audio backend, despite successful offline output. **[verified: commands, output WAV, generated.RPP]**

## 1. Philosophy: why it feels fast

The core interaction is a small set of composable objects: tracks, items, takes, envelopes, sends, FX and actions. The same track can hold media, receive audio, route to a parent, feed a sidechain and host effects; making a bus takes the same object as making a source track. Users can add a track with Ctrl+T, drag one track's I/O button to another to create a send, and make a submix by adding a parent track and toggling its folder button. **[verified: bundled Quick Start, pp. 1–2; generated.RPP]**

Fast editing relies on direct gestures and universal commands: right-drag marquee, drag item edges/fade handles, Alt-drag for slip or stretch, S to split, T to advance takes, `?` for the action list, and right-click discovery. **[verified: bundled Quick Start, pp. 1–3]** The default surface stays relatively sparse because specialized choices live in context menus, routing, properties, FX chains and the action list. **[from knowledge]** The application is known for a small installation and quick startup; this research did not benchmark footprint/startup, so Quire should measure its own latency rather than adopt a numeric target from this claim. **[from knowledge]**

Design implication: preserve immediate editing and broad object reuse, while exposing the current context and discoverable commands in Quire's own language. Configurability should not force users to learn hidden numeric action IDs. **[inference from verified workflows]**

## 2. RPP data model and text grammar

`<REAPER_PROJECT ...` is the root chunk opener. Indented `<TRACK {GUID}`, `<ITEM`, `<SOURCE MIDI`, `<FXCHAIN`, `<TAKEFX`, `<TEMPOENVEX` and `<RENDER_CFG` open nested chunks; a separate line containing `>` closes each chunk. Other lines are whitespace-separated keys and fields; quoted strings preserve spaces, and plug-in state may be opaque encoded lines inside an FX chunk. Top-level order in the generated file is project settings, master/tempo state, markers, then tracks in timeline order. Within each track, REAPER writes properties, receives, main send, then items/FX. **[verified: generated.RPP; normalization of minimal RPP]** RPP is a flexible serialization, not a formally documented interchange schema here; unknown lines and chunk ordering should be treated as potentially significant on import. **[inference]**

A track's opening GUID and `TRACKID` agreed in the generated file. Each item has an `IGUID`, while each take has a `GUID`; MIDI sources and FX instances have further GUIDs (`POOLEDEVTS`, source `GUID`, `FXID`). On the hand-authored round trip REAPER replaced the supplied opening track GUID, retained the supplied take `GUID`, and supplied the omitted item/source/master IDs. Thus an importer must distinguish identity domains and not assume every textual GUID is stable through normalization. **[verified: hand-authored project versus REAPER resave; generated.RPP]** The root line's timestamp-like field changed, and REAPER inserted many default lines. A byte-identical save is therefore not a property of REAPER; Quire's format should explicitly guarantee deterministic ordering and stable IDs, while separating app/UI state from musical state for useful diffs. **[verified: round-trip diff; inference for Quire]**

Concrete key locations from the generated file:

| Object | Serialized evidence | Consequence |
|---|---|---|
| Project | `TEMPO`, `<TEMPOENVEX` with `PT`, `MARKER`, `RENDER_*`, `<RENDER_CFG` | Tempo, structure and export settings are project state. **[verified: generated.RPP]** |
| Track/folder | `<TRACK`, `NAME`, `ISBUS`, `FIXEDLANES`, `NCHAN`, `MAINSEND`, `AUXRECV`, `TRACKID` | Folder depth is positional; reconstruct a tree by walking tracks. **[verified: generated.RPP]** |
| Item/take | `POSITION`, `LENGTH`, `FADEIN/FADEOUT`, `IGUID`, repeated take `NAME`/`PLAYRATE`/`GUID`/`<SOURCE`, `TAKE SEL`, `SM` | One item owns multiple source takes; active take and source timing are separate from item timing. **[verified: generated.RPP]** |
| FX | `<FXCHAIN`/`<TAKEFX`, `<VST ...`, opaque state, `BYPASS`, `FXID` | Store a stable FX instance identity separately from plugin ID and opaque preset blob. **[verified: generated.RPP]** |
| MIDI | `<SOURCE MIDI`, `HASDATA 1 960 QN`, `E` event lines, source GUID | Embedded MIDI is event data attached to a take source, not a separate global track. **[verified: generated.RPP]** |

`E` lines in this sample are delta PPQ offsets followed by MIDI status/data bytes: note on `E 0 90 30 64`, note off `E 960 80 30 00`, CC `E 0 b0 01 40`. The file also contains a generated all-notes-off event. An `E` is an ordinary selected/unspecified event here; REAPER also uses `e` and `X` variants for other event forms/selection, but their exact flags and all MIDI packing cases were not tested. **[verified: generated.RPP for `E`; from knowledge for `e`/`X`]**

The generated tempo envelope has points at 0 s/120 BPM, 4 s/132 BPM with a ramp flag requested through `SetTempoTimeSigMarker(..., true)`, and approximately 7.636 s/132 BPM with a 3/4 meter. The extra `MARKER` at approximately 4.909 s appeared after the tempo edit; do not assume a time signature edit maps to one simple line. **[verified: script inputs and generated.RPP]**

A complete text format should make sends explicit edges by stable track IDs rather than target indices, assign IDs to tracks/items/takes/FX/envelopes/markers, preserve opaque plugin state without leaking it into unrelated diffs, and canonicalize floats/ordering. The `AUXRECV` source indices in this one sample match zero-based track positions; a second reorder experiment is needed to prove how REAPER rewrites them. This makes reordering a concern for external text editors. **[verified: generated.RPP for index-position match; inference for reordering]**

## 3. Universal tracks and folders

The Lua API accepted `I_FOLDERDEPTH` values +1 for folder starts and -1 for the final child; REAPER saved `ISBUS 1 1` on each start and `ISBUS 2 -1` on the final child. A plain track saved `ISBUS 0 0`. `FIXEDLANES` appeared on most created tracks, including tracks without lane content; do not treat the presence of the key alone as proof of active comp lanes. **[verified: script input and generated.RPP]** `NCHAN 4` on the sidechain destination and `MAINSEND 0 0` on the return show audio channel count and master/parent send are independent properties. **[verified: generated.RPP]**

Any track can be a source, bus, folder, FX return or hardware-output source. Parent send and explicit sends can coexist; the folder is a track with children, not a second kind of bus. Track templates save a reusable track setup and are inserted via Track > Save/insert track template. **[verified: Quick Start for folder/template; generated.RPP for combined properties]** Folder compact states and hardware-out serialization were not exercised here. **[from knowledge]** FX pin mappings can connect the track's channels to arbitrary plug-in pins; the 3/4 send in the sample demonstrates the track-side routing but not a saved FX pin map. **[from knowledge plus verified 3/4 destination]**

## 4. Routing and sidechains

A common path is: make destination track four channels; create a send from a source; set that send's destination channel pair to 3/4; configure a compressor's detector pins/input to those channels. The sample exercises the first three steps, with source Kick, destination Sidechain return, `NCHAN 4`, and `AUXRECV ... 2 ...` on the destination. ReaComp is inserted, but its detector configuration and pin map were not changed, so the sample is **not** evidence of audible sidechain compression. **[verified: generated.RPP and Lua inputs; from knowledge for detector step]**

REAPER exposes sends and receives in a track routing window and a routing matrix; drag/drop from an I/O button is a fast send gesture. The matrix is useful for seeing many track-to-track and hardware connections at once. **[verified: Quick Start for drag gesture; from knowledge for window/matrix]** Send tap points include post-fader/post-pan, pre-fader/post-FX and pre-FX. We created modes 0, 1 and 3 with `I_SENDMODE`; their numeric-to-label mapping is from API knowledge, not independently checked in the UI. The destination file stores incoming routes as `AUXRECV`, so reconstructing the graph requires resolving receives against ordered tracks. **[verified: generated.RPP; from knowledge for mode labels]**

## 5. Items, takes, lanes and media

Items have their own project position/length, loop state, fades and item GUID. An item may contain successive take records, each with name, source, offset, rate, pitch-preservation setting, stretch markers and FX. In `generated.RPP` the same audio file appears as Take A at rate 1.25/preserve pitch and Take B at rate 0.8/no preserve pitch; `TAKE SEL` selects B; `SM 0.5 0.4` is its stretch marker; `FADEIN 1 0.1 ...` and `FADEOUT 2 0.25 ...` encode 100/250 ms fades plus shape fields. **[verified: generated.RPP and script inputs]** Fade shape numeric values should remain opaque until separately mapped from UI experiments. **[inference]**

Users can select takes by clicking lanes, advance with T/Shift+T, crop to active take, explode takes, add take FX, and create take envelopes. Item edges, fade handles, and crossfade gestures allow editing without opening a modal property sheet. **[verified: Quick Start, pp. 2–3]** REAPER 7 fixed lanes provide a separate comp workflow with lane visibility and comp regions; our file includes default `FIXEDLANES` keys but contains no actual comp area. **[from knowledge; limitation verified from generated.RPP]**

Item, track and project timebase can be chosen among time, beats and beats with position-only behavior; this affects how tempo changes move or stretch media. The `BEAT -1` key appears on generated tracks, but its exact encoding and per-item override were not tested. **[from knowledge; file observation for `BEAT`]** Mixed media formats and sample rates are generally accepted and resampled at playback/render; this sample only uses a 44.1 kHz WAV plus MIDI. **[from knowledge; verified sample scope]** REAPER uses `.reapeaks` companion caches to draw waveforms quickly; this run did not measure peak generation or cache invalidation. Quire should use background, disposable peak caches keyed by media identity/content and resolution, keeping them out of the canonical project diff. **[from knowledge; design inference]**

## 6. Actions, shortcuts and mouse modifiers

The bundled guide locates the searchable Actions list at `?`; users can assign keyboard/MIDI shortcuts and add actions to menus or toolbar buttons. **[verified: Quick Start, p. 3]** Native actions use numeric command IDs (for example the project-exit action `40004` worked in this run). ReaScript/custom actions use generated named identifiers such as `_RS...`; SWS extension actions use `_SWS...` when that optional extension is present. Scripts and custom actions can be composed into macros; cycle actions are an extension/workflow concept rather than demonstrated by this installation. **[verified: command 40004 run; from knowledge for identifier families and cycle actions]**

There is no `reaper-kb.ini` in Nate's current resource directory, so a full exported action-list count is unavailable. `KeyMaps/DK keymap.ReaperKeyMap` has **21** `KEY` lines; the German file has **23**. These are keymap entries, **not** counts of installed actions. Observed line form is `KEY modifier-code key-code command-ID section-ID`; for example `KEY 5 186 40223 0`. The exact modifier bit encoding was not tested. `ACT` custom-action and `SCR` script-registration records, and section IDs for main/MIDI editors, are known format elements but absent in this configuration and not verified locally. **[verified: file counts and lines; from knowledge for `ACT`/`SCR` and section meaning]**

`reaper-mouse.ini` contains 22 `MM_CTX_*` context defaults, including arrange middle/right mouse, item click/double-click/edge, envelope point/segment, MIDI note/piano roll/CC/ruler and track. Two explicit `mm_*` assignments appear under `MM_CTX_MIDI_NOTE_CLK`. This shows mouse bindings are contextual, with the defaults represented compactly rather than duplicating every gesture. The integers' behavior was not decoded. **[verified: `~/.config/REAPER/reaper-mouse.ini`]**

## 7. FX chains and processing

Track FX and take FX serialize in distinct chunks, `<FXCHAIN` and `<TAKEFX`. Each FX has a plug-in identity, opaque state, `FXID`, bypass state and UI placement; the generated sample has ReaComp on a track and ReaEQ on Take B. **[verified: generated.RPP]** The FX chain UI supports reordering, floating a plug-in, chain save/load, wet/dry, per-FX and full-chain bypass, and parameter envelopes/MIDI learn. **[verified: Quick Start, p. 2]** `.RfxChain` files reuse chain serialization; none existed in this local resource directory, so exact file contents were not checked. **[from knowledge; verified absence locally]**

REAPER 7 FX containers nest effects, allowing routing and reusable subchains; input FX process recorded input, monitoring FX affect listening, and take FX process an item independently of track FX. Presets, offline and bypass are distinct: bypass keeps an instance in the graph, while offline unloads/avoids processing it. Those semantics and container serialization need a separate UI/project experiment. **[from knowledge]** Plug-in delay compensation aligns delayed processing in playback/render; input monitoring/record latency can still depend on settings and plug-in behavior. No latency test was made. **[from knowledge]**

The current `reaper.ini` has `[reaper] autonbworkerthreads=0`; no `anticipativefx`-named setting appears in this short config. This verifies one local key/value, **not** the default or full anticipative-processing policy. REAPER's anticipative FX processing can prepare non-live tracks ahead of playback, while record-armed/monitored paths have different scheduling constraints; per-track FX performance options exist. Exact REAPER 7.80 key names/defaults and per-plugin flags remain open. **[verified: local reaper.ini; from knowledge for behavior]**

## 8. Plug-in process isolation

REAPER offers native, separate-process and dedicated-process hosting for some plug-in types, trading lower crash blast radius against IPC overhead, UI quirks and potentially greater latency. A crashed bridge can sometimes leave the main DAW running, but recovery varies by plug-in and operating system. This was not crash-tested, and Quire should not promise identical isolation from this note. **[from knowledge]**

The local `reaper-vstplugins64.ini` is a scan cache: under `[vstcache]`, each line maps a filename to scan metadata, an ID and a display name (e.g. the bundled ReaComp entry). The CLAP cache has sections by binary with IDs/names. No per-plug-in bridge/firewall flag was identified in those observed lines, so their numeric/hex scan fields must not be interpreted as isolation settings. A plugin UI/config test is needed for where isolation choice persists. **[verified: read-only inspection of local caches]**

## 9. Render/export

The bundled guide demonstrates File > Render for master mix or selected-track stems and consolidation/export for media handoff. **[verified: Quick Start, p. 1]** The generated project stores `RENDER_FILE`, `RENDER_FMT`, `RENDER_1X`, `RENDER_RANGE`, `RENDER_RESAMPLE`, `RENDER_ADDTOPROJ`, `RENDER_STEMS`, `RENDER_DITHER`, `RENDER_TRIM`, and binary/encoded `<RENDER_CFG`. The local config remembers `render_pattern_0=$project`. A minimal hand-authored RPP with `RENDER_FILE`, `RENDER_PATTERN`, `RENDER_RANGE` and `RENDER_FMT` rendered offline; on resave, REAPER folded the chosen output into `RENDER_FILE` and omitted `RENDER_PATTERN`. Treat these fields as REAPER-specific state, not a stable export API. **[verified: generated.RPP, local reaper.ini, render and resave]**

Render source choices include master, stems from selected tracks, selected items, and region/region-render-matrix based outputs; bounds include entire project, time selection, selected regions and custom range. Render queue can defer multiple jobs; secondary output can produce another format in one job. Normalization can target peak, true peak, LUFS-I/S/M or RMS, with brickwall limiting. The locally printed `reaper -h` batch-converter syntax explicitly lists those six normalize modes and peak/true-peak brickwall modes, but the full Render dialog's 7.80 choice set was not inspected. **[verified: `reaper -h` for normalization modes; from knowledge for dialog sources/queue/bounds]**

Filename wildcards known from use include `$project`, `$track`, `$region`, `$marker`, `$item`, `$take`, `$filenumber`, `$year`, `$month`, `$day` and metadata-related variants. Only `$project` appears locally (`render_pattern_0`); exact 7.80 spelling, availability by source, escaping and newer wildcards need UI verification. Do not build Quire's templating syntax from this unverified list. **[verified: local reaper.ini for `$project`; from knowledge for remaining names]**

## 10. Recording

Arm a track, pick its input from the arm-button context menu, choose monitoring, and record with Ctrl+R/Space. Recording onto a track with an existing item can create takes; loop recording and the record mode (normal, time selection auto-punch, selected-item auto-punch) determine how passes are placed. The guide confirms arm/input/monitor, Ctrl+R, new takes and record-mode menu. Loop/punch details were not exercised with an audio device. **[verified: Quick Start, p. 1; from knowledge for loop/punch specifics]**

The sample project has `RECORD_PATH "Media" ""` in its normalized header and `REC ...` per track; the path is project-relative in this case. REAPER can keep media with a project or use configurable recording paths, so Quire should make the destination explicit and inspectable before record. **[verified: generated.RPP; from knowledge for configurable path]**

## 11. Automation

Volume and pan envelopes can be shown with V/P; envelope points can be added/deleted and segment curvature edited directly; the guide shows the track envelope button for automation mode. **[verified: Quick Start, p. 2]** REAPER modes include trim/read, read, touch, latch, latch preview and write. Automation items are reusable/movable spans of envelope data, including pooled forms; envelope points have shapes such as linear, square, slow start/end and Bézier with tension. These mode/shape details were not generated or UI-checked. **[from knowledge]**

The generated tempo map is a `<TEMPOENVEX` with three `PT` lines, and the normalizer supplied `EGUID`, visibility, arm and default-shape fields even to the small seed project. That is evidence that envelopes have identity and edit/display metadata distinct from their point list. Quire should model automation points, interpolation, automation-item identity/pooling and write mode explicitly, with write operations represented as undoable transactions. **[verified: generated.RPP; design inference]**

## 12. MIDI editor and storage

Double-click a MIDI item to open the editor; E opens an inline editor. The native editor provides piano roll, velocity and CC lanes, an event list and notation view. Only the first two navigation gestures are in the bundled guide; alternate views were not opened. **[verified: Quick Start, p. 2; from knowledge for views]**

The sample's MIDI take has `HASDATA 1 960 QN`, CC and note events in `E` lines, `CCINTERP`, `POOLEDEVTS`, source GUID and `IGNTEMPO`. The header indicates a 960-PPQ QN basis in this file; event deltas sum to absolute PPQ positions. The file contains a note-on at 0 and note-off 960 ticks later. The selected-state `e` form and `X` text/sysex variants were not produced. **[verified: generated.RPP and Lua insertion; from knowledge for `e`/`X`]** Treat MIDI note identity and overlapping same-pitch notes carefully in Quire; a typed event model should expose channel, pitch, velocity, timing, selection and source identity without requiring raw hex parsing by clients. **[design inference]**

## 13. Customization

Users can put actions into menus and toolbar buttons via Options > Customize menus/toolbars; track templates and FX chains are reusable configuration units. **[verified: Quick Start, pp. 1–3]** REAPER also supports screensets, layouts and WALTER-scripted themes, with `reaper-menu.ini` for customized menus and resource-directory files for persistent UI configuration. The current resource directory has no `reaper-menu.ini`; no theme assets or WALTER scripts were read. **[from knowledge; verified absence of menu file]** Quire should copy the workflow's custom command placement and workspace recall while providing a stable, documented schema instead of requiring theme script knowledge. **[design inference]**

## 14. ReaScript API and lessons for a command bus

The temporary Lua script used `MediaTrack*`-style handles from `GetTrack`, `MediaItem*` from `AddMediaItemToTrack`, `MediaItem_Take*` from `AddTakeToMediaItem`, numeric `SetMediaTrackInfo_Value` keys (`I_FOLDERDEPTH`, `I_NCHAN`, `B_MAINSEND`), string `GetSetMediaTrackInfo_String(...,'P_NAME',...)`, `CreateTrackSend`/`SetTrackSendInfo_Value`, `SetTempoTimeSigMarker`, `MIDI_InsertNote`, and `Main_SaveProject`. REAPER saved the intended objects and exited via `Main_OnCommand(40004,0)`. This directly verifies the practical object-handle/typed-by-string-key style and procedural mutation model. **[verified: run result and generated.RPP]**

`GetSetProjectInfo_String` and other `GetSetMedia*` accessors use string keys whose valid values and units vary by object; `Main_OnCommand` takes numeric action IDs. A script can read a track, write a numeric field, then call `UpdateArrange`, but a headless client must know which thread/project is current and often poll for changes. Undo blocks require explicit `Undo_BeginBlock`/`Undo_EndBlock` discipline. ReaScript is powerful but its untyped field names, global current-project/selection state, synchronous imperative calls and limited event subscriptions make a remote automation client fragile. **[from knowledge; verified examples for key/action style]**

Quire's command bus should instead accept typed commands with stable object IDs, explicit project scope, version/precondition, structured errors, atomic undo transaction and an event stream carrying committed changes. For example, `CreateSend{sourceTrackId,destinationTrackId,tapPoint,destinationChannels=[3,4]}` should return a send ID; `SetItemTakeRate{itemId,takeId,rate,preservePitch}` should validate rate and publish a precise change event. A parameter should have a named unit/range, not only an anonymous normalized float. **[design inference]**

## 15. Weak spots Quire can improve

- Scripting APIs expose stringly-typed keys, numeric IDs, current selection and manual undo scaffolding; remote clients need stable schemas, transactional commands and subscriptions. **[verified: sample ReaScript for string keys/IDs; from knowledge for broader limits]**
- Plug-in parameters are often surfaced as normalized 0–1 values with inconsistent names and no shared semantic units. A typed parameter schema could let agents ask for “threshold −18 dB” without guessing an index or conversion. **[from knowledge; design inference]**
- A/B comparison across arbitrary project states, render candidates or FX settings is possible via snapshots/workarounds but lacks one universal, inspectable experiment object. Quire can make comparison sets and provenance first-class. **[from knowledge; design inference]**
- Built-in analysis and machine-readable measurement workflows are fragmented among meters, render statistics and plug-ins. Quire can attach repeatable analyses to a project revision/render with typed results. **[from knowledge; design inference]**
- Deep settings, context menus and modal routing/FX dialogs create discoverability costs. Searchable commands are a strength; explaining what a command will change is an opportunity. **[verified: Quick Start's repeated right-click/action-list advice; design inference]**
- RPP round trips add defaults/IDs and reorder or normalize state, making hand edits noisy. Quire can specify a canonical, diff-friendly project format with stable IDs and strict versioning. **[verified: minimal RPP resave diff; design inference]**

## 16. GUI-phase workflow priority: top 40

Priority tags: **P0** for daily editing/routing needed early, **P1** for serious production depth, **P2** for advanced customization. These are Quire design priorities, not REAPER feature rankings. **[design inference from the evidence above]**

1. **P0** Create a universal track instantly and rename it in place.
2. **P0** Turn a track into a folder/bus and show parentage clearly.
3. **P0** Drag a track output to another track to create a send.
4. **P0** Inspect and edit each send's source, destination, channels and tap point.
5. **P0** Show a routing overview that makes cycles/sidechains legible.
6. **P0** Set track channel count and route to channel pairs 3/4 and beyond.
7. **P0** Control parent/master send independently of explicit sends.
8. **P0** Drop mixed audio/MIDI files directly onto tracks and timeline.
9. **P0** Move, trim, split and duplicate items with direct gestures.
10. **P0** Draw fades and change crossfades at item edges.
11. **P0** Slip media inside an item without moving the item.
12. **P0** Stretch an item while making rate/pitch mode visible.
13. **P0** Switch takes and see the active take unambiguously.
14. **P0** Open a searchable command list from anywhere.
15. **P0** Assign shortcuts and show conflicts by context.
16. **P0** Add, reorder, bypass and wet/dry mix track FX quickly.
17. **P0** Render the master with clear source, bounds, format and destination.
18. **P0** Arm, pick input, monitor and record without hidden path state.
19. **P0** Save/reopen a project with stable IDs and readable diffs.
20. **P0** Undo/redo every edit and expose command results to automation.
21. **P1** Show takes in lanes and perform a fixed-lane comp.
22. **P1** Add take FX and item/take envelopes.
23. **P1** Draw/edit tempo ramps and meter changes on the timeline.
24. **P1** Create, rename and navigate markers and regions.
25. **P1** Edit MIDI notes with piano roll, velocity and CC lanes.
26. **P1** Provide MIDI event list and notation views.
27. **P1** Show track automation lanes and write/read modes.
28. **P1** Create automation items and preserve pooled identity.
29. **P1** Set project/track/item timebase with visible consequences.
30. **P1** Save and insert track templates and FX chains.
31. **P1** Inspect FX pin maps and connect detector input to 3/4.
32. **P1** Render selected tracks, items and regions as stems.
33. **P1** Queue multiple exports and support a second output format.
34. **P1** Offer true-peak/LUFS normalization and render measurements.
35. **P1** Loop record into takes and support punch modes.
36. **P1** Show/cache waveform peaks without blocking editing.
37. **P2** Customize contextual mouse gestures and toolbar/menu actions.
38. **P2** Recall screensets and track layouts.
39. **P2** Host plug-ins in isolated processes with visible recovery state.
40. **P2** Build reusable multi-action macros with typed inputs and undo.

## Open questions and next experiments

1. Capture a REAPER 7.80 exported action list, `reaper-kb.ini` with `ACT`/`SCR`, and custom mouse bindings to verify section IDs, named IDs and counts. Current keymap counts are only 21/23 entries.
2. Use a temporary config and GUI to save fixed-lane comp regions, automation items, item/take envelopes, FX containers, FX pin maps, hardware outputs and offline/bypass state; compare RPPs one action at a time.
3. Verify exact render-dialog sources/bounds/wildcards, region matrix, secondary format, normalization and queue representation in 7.80.
4. Measure peak-cache file creation and invalidation with larger mixed-rate media; test timebase changes against a tempo ramp.
5. Inspect plugin compatibility settings through UI and copied config, then crash-test a disposable plugin bridge; do not infer isolation from scan-cache numeric fields.
6. Inspect anticipative FX preferences in a fresh temporary config and establish key names/defaults and monitored-track behavior.
7. `ARCHITECTURE.md`, `STATUS.md`, ADRs and component specs were absent at research start; reconcile these recommendations when those documents are available.

## Addendum (2026-09-26, orchestrator): action inventory and a GUI pass

**Action inventory** [verified: `reaper.kbd_enumerateActions` over sections
0/32060/32061/32062/32063 in a temporary-config REAPER 7.80 run]: main
section **4,367** actions (section 100 enumerates the same list), MIDI editor
**1,103**, MIDI event list 299, inline MIDI editor 187, media explorer 293.
Largest main-section prefixes: Track 1,331, Automation 352, Group 256, Item
255, Item properties 204, View 133, Toolbar(s) 218, Options 118, Envelope 84,
Ruler 77, Markers 71, Transport 61, Track lanes 61, Project tabs 60.
Replacing digits with `N` collapses the main section to **2,018** distinct
action *shapes*: e.g. "Track: Toggle mute for track N" exists 99 times (one
per track number). Implication for Quire: parameterized typed commands
(`track.mute {track, value}`) plus REAPER-style *actions* = command + bound
arguments cover this surface with roughly an order of magnitude fewer
registry entries, and the actions list can still present bindable, named
entries (generated from command + argument presets).

**FX chain window** [verified: GUI, ReaComp on the sidechain-return track]:
chain list on the left with Add/Remove and a CPU/PDC readout ("0.01%/0.01%
0/0 spls"); per-FX header with preset menu, `Param` (learn/modulation/
envelopes), a pin-connector button labelled with the channel configuration
("4 in 2 out" — the 3/4 sidechain input), `UI` toggle, wet/dry knob and
bypass. The stock plugin UI already shows physical units (ms, dB, Hz, :1).
The FX context menu contains: Add/Replace FX, **Add container / Move FX to
container**, **Run selected FX in parallel with previous FX** (and a
merge-MIDI variant), **Build multi-mono / multi-stereo FX**, FX chains ▸,
Freeze track ▸, copy/cut/paste FX (with or without automation), remove, **FX
bypass (Ctrl+B) vs FX offline (Ctrl+Alt+B)**, rename FX instance (F2), float
window, **show embedded UI in TCP / MCP**, send all keyboard input to
plug-in, **Chain PDC mode ▸**, **Chain oversampling ▸, FX instance
oversampling ▸**, **Force auto-bypass on silence**, build multichannel
routing for outputs, build 16 channels of MIDI routing, dock/close. Process
isolation is not in this menu (it is chosen per plugin in the FX browser's
"Run as" submenu and in plug-in preferences; not re-verified in this pass).

Quire takeaways: per-FX and per-chain oversampling, parallel FX within a
chain, containers, per-FX PDC policy, bypass ≠ offline, and auto-bypass on
silence are all FX-chain *model* features (commands on `fx.*`), not GUI
extras — they belong in the Phase 4 session schema.

**XWayland note:** on this Hyprland setup REAPER's unfocused floating windows
and popup menus render translucent (compositor inactive-opacity applied to
X11 windows). Quire's X11 windows will get the same treatment; worth a
window-rule recommendation in Phase 5 docs rather than a code workaround.
