# Phase 4 capacity tooling (B1 / B2)

B1 builds the session through the engine command bus. B2 exports that model's
compiled notation MIDI and the plugins' own CLAP-saved states, then uses public
ReaScript APIs to generate and save an RPP. The recipe lives once in
`host/engine/bench/session.json`: 120 BPM, a four-bar eight-note chord loop,
Orrery's synth.md §9 reference patch → non-flat four-band Facet → Ballast on
N playback tracks; an additional armed stereo-input track with the same
chain, monitoring on; Ballast in limiter mode on the master. N excludes the
monitored track. Playback tracks use −18 dB faders. The export includes the
engine's pan law; REAPER receives it explicitly. Engine tracks use `track.add`
defaults and their default parent sends, so new strip types need no builder
rewrite. REAPER's anticipative FX preferences are left at their defaults.

Build an optimized tool and the three stock artifacts before capacity runs:

```sh
cmake --preset release
cmake --build --preset release -j6 --target engine-bench plugin-orrery plugin-eq plugin-comp
```

On the otherwise idle workstation, each command searches and independently
confirms the largest passing N, for 300 measured seconds **per trial**:

```sh
# B1: two searches, anticipative on and off.
SUPPLE_AUDIO_DEVICE_TEST=1 build/release/host/engine/engine-bench --device jack --search --anticipative both --seconds 300 --max-tracks 256 --report docs/benchmarks/b1.md
# B2: REAPER's default anticipative FX, same plugins/patch/MIDI.
SUPPLE_AUDIO_DEVICE_TEST=1 python3 tools/bench/run.py b2 --engine build/release/host/engine/engine-bench --device jack --search --seconds 300 --max-tracks 256 --report docs/benchmarks/b2.md
```

Both real modes require `SUPPLE_AUDIO_DEVICE_TEST` equal to exactly `1`.
Unset, empty, `0`, `01`, and all other values refuse before opening a device.
B1 uses JACK with `forceBufferSize` and sets `PIPEWIRE_QUANTUM=64/48000` in its
own environment (a user `jack.conf` with `node.force-quantum` otherwise wins:
on this workstation it forces 128, and the first real run was rejected as
invalid); it prints before requesting 64 frames for the PipeWire graph,
affecting other clients while it runs. B2 records JACK
only in a fresh private REAPER config and requests `PIPEWIRE_QUANTUM=64/48000`
in that child's environment only. Both validate the **granted** 48 kHz/64
settings; a different grant, server loss or processing error invalidates the
trial. Real input/output ports are connected for monitoring/playback. No
system configuration is written. Quantum restoration is the JACK/PipeWire
server's behavior, not a system-setting write by this tool.

Dry verification (no physical audio device):

```sh
SUPPLE_AUDIO_DEVICE_TEST=0 build/release/host/engine/engine-bench --device paced --search --anticipative both --seconds .2 --max-tracks 8 --report build/bench/b1-dry.md
SUPPLE_AUDIO_DEVICE_TEST=0 python3 tools/bench/run.py b2 --engine build/release/host/engine/engine-bench --device offline --tracks 2 --report build/bench/b2-dry.md
python3 -m unittest discover -s tools/bench -p 'test_*.py' -v
python3 -m unittest discover -s tools/reaper-check -p 'test_*.py' -v
```

`paced` supplies silent stereo input on a stand-in thread. Its `xruns` are
callback/scheduling deadline misses, not hardware xruns; anticipation misses
also fail a trial, including when the monitored/master path is live. B1 and
B2 exclude activation and a one-second preroll from the measured interval.
Short dry runs prove operation, **not** five-minute capacity or B1's ≥3×
target. Debug builds can have no passing N; the search records N=0 and exits
nonzero. The report marks the device, build facts and exact durations.

B2 always generates the project under Dummy audio, closes audio/MIDI, checks
every loaded CLAP identity and loads its opaque state using `clap_chunk`.
Offline mode executes `-renderproject`, validates an eight-second stereo
48 kHz WAV with finite, non-silent audio, and never starts playback on JACK.
It reuses `tools/reaper-check`'s allowlisted private config, license copy,
resource-path check, blocked servers, user-config fingerprints, timeout and
process-group cleanup. Real measurement uses a separate private JACK config;
no user's device preferences, caches or projects are copied or changed.
`--reaper /path/to/reaper` selects the executable. A desktop display is needed
for REAPER startup; no GUI input or proprietary source/assets are used.

The real B2 script polls
[GetUnderrunTime](https://www.reaper.fm/sdk/reascript/reascripthelp.html#GetUnderrunTime)
in deferred callbacks and counts changes to audio and media xrun timestamps.
These are observed events, **not an exact cumulative counter**: multiple
xruns between polls can collapse into one observation, and audio timestamps
are available only when the backend supplies them. Zero observed changes is
the acceptance predicate. Poll count and maximum poll gap, both last-event
timestamps, REAPER version and audio-device info are retained in the TSV/JSON.

Search doubles N to bracket failure, then bisects integer N and independently
repeats the candidate. This assumes capacity is monotonic on an idle machine.
A failed confirmation returns nonzero and is recorded as unconfirmed. If
`--max-tracks` passes, the result is **≥N**, not a claimed maximum; increase
that bound to find a failure. `--tracks N` without `--search` measures one
case. B2 offline mode has no capacity search.

Each invocation gets a fresh `build/bench/<UTC>-<random>/` with per-trial logs,
JSON, generated scripts, exported session, RPP/WAV and config fingerprints.
The final `report.json` has schema `supple.capacity-report/1.0`, all trials and
confirmation flags, exact command arrays, kernel, PipeWire version, CPU and
all governor values (read-only), revision/dirty status and CLAP artifact
hashes. No wall-clock threshold decides a unit test. Markdown rows append to
`docs/benchmarks/b1.md` or `b2.md`; `--report` redirects dry runs elsewhere.
`--output-dir` chooses a new artifact directory. Tool failures retain a
structured error and logs. Reports/license copies under build are not committed.

## Verification on 2026-09-30

Implementation commit: `31bb265`. No physical audio device was opened.

- `cmake --preset dev && cmake --build --preset dev -j6`: passed.
- `SUPPLE_AUDIO_DEVICE_TEST=0 ctest --test-dir build/dev -R '^(engine\.tests|engine\.bench-tooling)$' -j6 --output-on-failure`:
  both passed (54.11 s total); session builder/export had 34 assertions.
- The two unittest commands above: 5 benchmark tests and 11 REAPER-check tests passed.
- The optimized paced search command above: confirmed N=5 with anticipation
  on, N=1 off, at 0.2 s per trial; exercised bracketing, bisection and confirmation.
  Artifacts: `build/bench/20260930-164138-9724cdd8/{True,False}/report.json`.
  These are dry checks, not acceptance capacities. An earlier debug search
  correctly recorded no passing N rather than claiming a capacity.
- The optimized B2 offline command above: REAPER 7.81 loaded/read back all
  ten CLAP states at N=2, saved the RPP, and rendered 384000 stereo frames
  at 48 kHz, finite and non-silent. User-config fingerprints were unchanged.
  Artifacts: `build/bench/20260930-164124-bc02f432/REAPER-default/`.
- `SUPPLE_AUDIO_DEVICE_TEST=0 build/dev/host/engine/engine-bench --device jack --tracks 1 --seconds .1`:
  refused with `bench.opt_in` before device start.
- `env -u SUPPLE_AUDIO_DEVICE_TEST tools/ci/run-all.sh`: run once; dev, GCC,
  ASan, UBSan, TSan, RTSan, license and format all passed. Logs are retained
  under `build/ci-*.log`; temporary sanitizer/GCC trees were removed.

Real-device paths and five-minute acceptance measurements are left for the
orchestrator, as requested, after the Console 9 strips land.
