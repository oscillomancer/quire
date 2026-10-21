# REAPER CLAP acceptance

On-demand Linux REAPER load, offline render, automation and state checks for
stock CLAP artifacts, with an optional floating-editor check. Python 3.10+, standard library only. REAPER is a local
runtime requirement, **not a build dependency**. This tool is deliberately
not registered with CTest or `tools/ci/run-all.sh`.

```sh
cmake --preset dev
cmake --build --preset dev -j6
python3 tools/reaper-check/check.py --build-dir build/dev
# Open, confirm and close floating editors (requires a desktop display):
python3 tools/reaper-check/check.py --build-dir build/dev --editor
# A focused run, another preset or an explicit executable:
python3 tools/reaper-check/check.py --build-dir build/release --plugin com.suppleaudio.hickory
python3 tools/reaper-check/check.py --reaper /usr/lib/REAPER/reaper --timeout 30
# Pure helper/process tests: no REAPER or audio device required.
python3 -m unittest discover -s tools/reaper-check -p 'test_*.py' -v
```

The runner checks every `*.clap` in the selected build's `plugins/clap/`.
Unknown artifacts fail with a request to add a `CASES` entry; they are not
silently omitted. `--plugin` is repeatable and missing requested artifacts
fail. Without that filter, future table entries whose binaries are absent
are not counted as tested. Each report records the discovered paths and
SHA-256 hashes, so a retained artifact from an earlier branch is identifiable.

Outputs go to a fresh `build/reaper-check/<timestamp>-<random>/` directory:
`report.json` (schema, REAPER version, exact command arrays, table, measurements
and errors, plus editor status/reason when requested), console table, logs,
generated scripts, REAPER-saved RPPs, WAVs
and configuration checksum listings. Exit 0 means all selected artifacts
passed; any failed check, timeout, launch error, interruption or user-config
change returns nonzero. A missing REAPER executable/build is an argument error.
Paths/reasons are printed; structured JSON errors include code/message/path/hint.
No report or user license belongs in git.

## Isolation and lifetime

The [B1/B2 capacity tool](../bench/README.md) reuses these isolation helpers.
Its separate real JACK mode requires the exact `SUPPLE_AUDIO_DEVICE_TEST=1`
opt-in before creating a device config or launching a process; this
acceptance check's invocations continue to use the default Dummy configuration.

Only a fresh allowlisted private `config/reaper.ini` and, when present, a copy
of `~/.config/REAPER/reaper-license.rk` seed the resource directory. The license
copy has mode 0600 and its parent has mode 0700. User devices, projects, plugin
caches, preferences and startup scripts are not copied. CLAP scans only the
selected artifact directory; VST/LV2 paths point to an empty private directory.
`-newinst -nosplash -cfgfile <private ini>` prevents reuse of another instance.
`GetResourcePath()` must confirm the private directory before setup proceeds.

The Linux dummy system is selected by `linux_audio_mode=2` (REAPER's Linux modes,
probed 2026-10-01: 0 JACK, 2 Dummy Audio, 3 PulseAudio; 1 and 4 open nothing
without further device settings); the script checks
`GetAudioDeviceInfo('MODE')` and accepts only Dummy or an already closed device,
then calls `Audio_Quit()` (also closes MIDI). Offline renders use
`-renderproject <absolute RPP>` and exit. Private MIDI input/output masks are
zero, and JACK/PipeWire/Pulse server environment settings prevent an accidental
connection to the workstation's servers. Editors are opened only with `--editor`.
REAPER's main/render/editor windows may appear briefly; the tool uses public
ReaScript calls and never synthesizes desktop input.

Every invocation has a timeout (default 20 seconds) and a new process group.
The group is terminated and then killed on timeout/interruption; scanner
children in that group are also killed, including after a normal parent exit.
Existing REAPER instances are never signalled. A helper regression test spawns
a parent and child and verifies timeout cleanup.

Before and after **each** REAPER invocation, and around the entire suite, the
tool records the user resource tree's relative paths, sizes, nanosecond mtimes,
file types, SHA-256 contents and symlink targets. Access times are ignored
because reads change them. Any difference stops the suite loudly, preserving
the two listings; it does not attempt to restore or overwrite user files.
Concurrent user edits to that tree also correctly trip the guard.

## Optional editor acceptance

`--editor` adds one invocation per plugin, reopening the saved default project
under the same private configuration, timeout, process-group cleanup and user
configuration guards. `TrackFX_Show(...,3)` requests a floating editor. Deferred
callbacks let REAPER process window events; both `TrackFX_GetFloatingWindow()`
and `TrackFX_GetOpen()` must confirm it is shown. The script then hides the
floating window and chain and confirms both indicators are closed before
quitting. Confirmation is bounded to three seconds; the outer process timeout
still bounds a hung plugin/host call. Errors also attempt to close the editor.

The console/report add an `editor` pass/fail/skipped column and an
`editor_reason`. Without the flag, editor acceptance is skipped with the reason
`--editor not requested`. A requested editor failure makes the whole run fail. Once the default project
loads, editor acceptance runs even if signal or automation acceptance fails.
A display server is required for this mode. It verifies REAPER's floating UI
state, not pixel appearance or control interaction. Screenshots are not captured;
no desktop capture dependency or REAPER extension is needed. Logs and the
`editor.tsv` host observations are retained beside the projects.

## Fixtures and expectations

Each project has one stereo track and a four-second render at 48 kHz. Effects
receive an original, seeded Voss octave-noise WAV plus a continuous 1 kHz tone
and windowed 125 Hz/8 kHz bursts. Its two-second halves repeat exactly.
Instruments receive repeated four-note MIDI phrases. Default, bypass and
automation projects are saved by REAPER, then rendered in separate processes.

All outputs must have exactly 192000 frames, two channels, the requested rate
and finite decoded samples. The reader handles PCM 8/16/24/32 and IEEE float
32/64 (including extensible WAV); the generated render configuration uses
24-bit PCM, normalization/dither/tail off. This checks the output file rather
than asserting the plugin's internal floating-point safety.

Default output must be non-silent. Instrument bypass must be silent. Hickory,
Lichen, neutral Gorse, Yarrow (every band off), Treeline (no gain, the
signal under its ceiling) and Sorrel (the fixture above its threshold) must
match bypass sample-for-sample; other defaults must differ. Dynamics differences may
include startup ramps.

| CLAP id suffix | Artifact / host name | Automated parameter | Points at 0 / 2 seconds | Expectation |
|---|---|---|---|---|
| `orrery` | Orrery | Output Level | 0 / −12 dB | −12 ±1 dB |
| `yarrow` | Yarrow | Output | 0 / −12 dB | −12 ±1 dB |
| `sundew` | Sundew | Threshold | 0 / −36 dB | second half >3 dB quieter |
| `treeline` | Treeline | Ceiling | 0 / −24 dB | second half >3 dB quieter |
| `gorse` | Gorse | Output | 0 / −12 dB | −12 ±1 dB |
| `hickory` | Hickory | Gain | 0 / −12 dB | −12 ±1 dB |
| `sorrel` | Sorrel | Threshold | −80 / −6 dB | second half >3 dB quieter |
| `teasel` | Teasel | Output | 0 / −12 dB | −12 ±1 dB |
| `bracken` | Bracken | Output | 0 / −12 dB | −12 ±1 dB |
| `canopy` | Canopy | Output | 0 / −12 dB | −12 ±1 dB |
| `lichen` | Lichen | Target | −30 / −5 LUFS | envelope reload + unchanged audio |

Continuous values use ADR-0010's normalized CLAP domain. The table keeps those
values explicit. Add a plugin in a few lines in `CASES`
(id, artifact name, instrument/effect, parameter, endpoints and
expectation). No plugin C++ or REAPER installation changes are required.

Automation is a square step at 2 seconds, with a terminal point at 4 seconds.
RMS is measured in 0.35–1.8 and 2.35–3.8 second windows; each is divided by the
same window of the unautomated render before taking their ratio. This cancels
phrase/tail/detector differences without assuming stationary input. Lichen's
Target envelope is saved and reloaded and its audio transparency is measured;
**its readouts are not measured** by this offline audio check.

## Parameter addressing and saved state: observed in REAPER

Tested locally with **7.81/linux-x86_64**. The earlier research route used 7.80:
[reaper-ux.md](../../docs/research/reaper-ux.md) and
[methods.md](../../docs/research/reaper/methods.md). Setup uses the installed CLI, public
[ReaScript APIs](https://www.reaper.fm/sdk/reascript/reascripthelp.html) and
REAPER-saved projects. No proprietary implementation or assets are inspected
or copied.

`TrackFX_GetParamName()` finds the table's unique display name;
`TrackFX_GetParamIdent()` records its host identifier. `GetFXEnvelope(track,
fx,index,true)` takes the **zero-based REAPER parameter index**, not the CLAP
id. REAPER saves `<PARMENV index:CLAP-id min max default`, e.g. Plumb (the
utility Hickory replaced) Gain `0:0`, Facet (the EQ Yarrow replaced) Band 3
Gain `17:17`, and Orrery Output Level **`219:2500`**.
The Orrery observation distinguishes index from id. Indices may change with
parameter enumeration; runtime discovery avoids assuming index equals id.
The script reopens the saved automation RPP and checks all three envelope
points at the resolved parameter before reporting success. The actual headers,
parameter enumeration and endpoints are retained in `metadata.tsv` and RPPs.

Headless resaving works: `Main_SaveProjectEx(...,8)` sets the project filename;
`Main_openProject('noprompt:' .. path)` reloads it. State acceptance sets a
non-default value, performs one offline render so REAPER delivers queued CLAP
parameter edits, saves, reloads and checks the value before resaving. Saving
immediately after `TrackFX_SetParam()` can otherwise serialize the old plugin
blob while the host's UI cache already shows the new value. Saved projects
contain REAPER's `<CLAP ...><STATE ...>` opaque payload; the tool does not invent
or decode that format. This verifies one changed parameter's state round-trip,
not every parameter or cross-version migration. A saved empty project and a
deferred File: Quit action close setup without a save prompt.
