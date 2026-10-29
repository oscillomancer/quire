# Spec: Coppice — the stock sampler

**Status:** Implemented: the three play modes, the voice, its modulation
and its editor, with seven factory samples. The sampler guide
(`skill/guides/sampler.md`) describes it for agents.
**Plugin:** `plugins/coppice`, CLAP id `com.suppleaudio.coppice`,
instrument. A DSP script compiled natively (ADR-0047),
`plugins/coppice/script/coppice.cpp`.

Coppice plays one sound, a file of the user's or one of its factory
samples, three ways: pitched across the keys with a sustain loop
(classic), whole at each key (one-shot), or cut into slices, one a key
(slice). It has no predecessor. It keeps Marram's keys, filter, envelopes,
LFOs, routes and look wherever it has the same thing, so an agent that
knows one knows most of the other. Zones, velocity layers, round robin,
time-stretching, recording, reading a file's root or `smpl` loop points
and detecting its root are left out (the design's reasons are in the
commit that added it); a drum sampler with several pads is a separate
plugin, still to come.

## 1. Signal flow (per voice)

```
 note ── region (start..end, reversed or not) ── a playhead, its speed from the key,
         │                                       root, transpose, tune, env2, bend, glide
         │ classic: the loop (off, forward with a crossfade, ping-pong), held while the key is
         │ one-shot: the region to its end      slice: slice k, trigger / gate / thru
         ▼
   read (cubic, at 1x/2x/4x) ── filter (off, ladder, svf; drive) ── amp (env × velocity
   × the note's volume, pan) ── voices summed ── down to 1x ── −9 dB ── output ── safety limit
 env2 (pitch and cutoff by fixed amounts), lfo1, lfo2, macro1..4 and routes (§3)
```

- **Files** arrive resampled to the session's rate, so the root plays at
  the speed of 1, and are measured as they load (the runtime's `peak()`
  and `rms()` over any range of frames): a slice's place, the waveform and
  Normalize's level are ready in the block the file arrives in. A voice
  reads as many channels as the file has: a mono file costs one.
- **Oversampling** as Marram's (`voice.quality`: eco 1x, normal 2x, high
  4x; minimum-phase half-bands, 3 to 5 samples of latency, reported); a
  sound pitched up stays free of aliasing an octave up at normal, two at
  high.
- **Control** every 16 samples of a voice's life, glided between, as
  Marram's, so a render does not depend on the block size; free LFOs count
  whole frames from their anchors.
- **Voices:** 32 (`voice.polyphony` plays fewer in poly). A stolen voice,
  or every voice when the file changes, fades out over a few milliseconds;
  a region's last millisecond fades out where playback stops at it. The
  start has no fade, so a drum's attack stays sharp (`amp.attack` softens a
  start in the middle of a sound).
- **Level:** a fixed −9 dB before `output` gives a chord room: the factory
  samples peak at −3 dBFS, so the default's single note peaks near
  −16 dBFS and `dsp.test`'s chord near −7, about as Marram's (−17 and −9).
  Marram's safety limit then leaves the output untouched below −1 dBFS and
  bends it toward full scale above.
- **Tail** (CLAP's `tail`): the longest amp release, a voice's routes
  included, or what is left of a note that plays on whatever its key does
  (one-shot, slice trigger, play on) at its speed; plus 20 ms.

## 2. Parameters

Keys are the patch keys; units as in every semantic plugin. Defaults in
**bold**. "Applies" is the parameter's activity condition: an inactive
parameter keeps its value and is left out of descriptions. The file is the
property `sample` (§4).

### 2.1 `sample`

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `sample.mode` | `classic`, `one_shot`, `slice` | | **classic** | |
| `sample.start` | % of the file; routes move a note's own start, read at its start | 0..100 | **0** | |
| `sample.end` | % of the file | 0..100 | **100** | |
| `sample.reverse` | the region played from its end to its start | | **off** | |
| `sample.normalize` | the file's peak brought to 0 dBFS | | **off** | |
| `sample.gain` | dB into the voice | −24..24 | **0** | |
| `sample.root` | the key that plays the file as recorded | 0..127 | **60** | mode not slice |
| `sample.transpose` | st | −48..48 | **0** | |
| `sample.keytrack` | keys change the pitch; off, every key plays the root's | | **on** | mode not slice |

The region is start..end (an end before the start plays the frames
between them). A slice keeps its recorded pitch, moved only by transpose,
tune, env2, bend and routes.

### 2.2 `loop` (classic)

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `loop.mode` | `off`, `forward`, `ping_pong` | | **off** | mode classic |
| `loop.start`, `loop.end` | % of the file, kept inside the region | 0..100 | **50**, **100** | loop on |
| `loop.crossfade` | ms, equal-gain, at most half the loop | 0..1000 | **20** | forward |
| `loop.snap` | the ends moved to the nearest rising zero crossings within ±5 ms | | **on** | loop on |
| `loop.release` | once the key is up, leave the loop and play on to the region's end ("Play On") | | **off** | loop on |

A note plays from the start into the loop and around it while its key is
held; at the key's release the amp envelope releases where it is (or,
with `loop.release`, the playhead goes on to the end). Reversed, the loop
runs backwards too. Without a loop a classic note stops at the region's
end or its release, whichever is first.

### 2.3 `slice` (slice)

| key | unit / options | range | default | applies |
|---|---|---|---|---|
| `slice.by` | `transients`, `grid` (equal parts) | | **transients** | mode slice |
| `slice.sensitivity` | % (more: quieter hits count) | 0..100 | **50** | transients |
| `slice.count` | equal slices | 2..64 | **16** | grid |
| `slice.key` | the first slice's key; the next slices on the keys above | 0..127 | **36** | mode slice |
| `slice.play` | `trigger` (to the slice's end), `gate` (while the key is held), `thru` (on through the rest of the region while held) | | **trigger** | mode slice |

**Transients**, at most 64: the region's level in hops of
max(256, length/4096) frames, a hit where it jumps by 3 − 2.4 × sensitivity
(a ratio) over the level two or three hops before and stands above the
loudest hop by −(12 + 36 × sensitivity) dB, at least 50 ms after the last;
then placed to the frame by halving with `peak()`, and moved back to the
zero crossing before it. The region's start is always the first slice. A
key past the last slice is silent. Mono voice mode gives the classic chop,
each slice cutting off the last.

### 2.4 `voice`, `filter`, `amp`, `env2`, `lfo1`, `lfo2`, `macro1`..`macro4`, `output`

As Marram's (`marram.md` §2), with these differences:

- `voice`: `voice.polyphony` 1..**32**, default **16**; no unison or drift.
- `filter`: one, Marram's filter 1 without comb and formant:
  `filter.type` `off` (**default**), `ladder`, `svf`; `filter.cutoff`,
  `filter.resonance`, `filter.slope` (ladder), `filter.mode` (svf),
  `filter.keytrack`, `filter.env`, `filter.velocity` and `filter.drive`
  with Marram's ranges and defaults, but `filter.env` **0**.
- `amp`: attack **0**, decay **1000**, sustain **100**, release **100**
  ms; the release does not apply to a note that plays on whatever its key
  does (one-shot, slice trigger).
- `env2`: attack **1**, decay **400**, sustain **0**, release **300**;
  `env2.pitch` st at its peak; no `env2.fm`; no Env 3.
- Two LFOs (`lfo1.rate` **2** Hz, `lfo2.rate` **0.5** Hz).

## 3. Modulation

The patch's `routes` member, as Marram's (`marram.md` §3).

**Sources.** Per voice: `amp`, `env2` (0..1), `lfo1`, `lfo2` (−1..1), and
an instrument's own `velocity`, `key`, `random`, `pressure` and `slide`.
For all voices: `macro1`..`macro4` and `modwheel`, `breath`, `expression`,
`aftertouch`, `pitchbend`.

**Destinations** (every one per voice): `sample.start` (read at a note's
start: `"velocity>sample.start": {"amount": "20 %"}` starts harder notes
later), `voice.tune` (reaching ±4800 ct), `filter.cutoff`,
`filter.resonance`, `filter.drive`, `amp.level`, `amp.pan`, the attack,
decay and release of the amp and Env 2, and each LFO's `rate`.

## 4. The sample and the factory samples

The property `sample` is a file reference: a path, or `factory:<name>`
for one of the files inside the plugin (`semantic-params-extension.md`,
"Factory files", 2026-10-29). A new
instance plays `factory:keys.flac`. Seven factory samples, 48 kHz 16-bit
FLAC, 918,861 bytes in all, are rendered from Marram by
`tools/factory-samples/render.py` from `plugins/coppice/samples/recipes.json`
(reproducibly: `--check` renders them again and compares the bytes);
first-party content, CC0 (`plugins/coppice/samples/LICENSE`). Each peaks at
−3 dBFS.

| name | what | root | channels, length |
|---|---|---|---|
| `keys.flac` | an FM electric piano's note | C4 (60) | mono, 4 s |
| `pad.flac` | a detuned saw pad | C3 (48) | stereo, 4 s |
| `pluck.flac` | two saws plucked through the ladder | C4 (60) | mono, 1.8 s |
| `bass.flac` | a saw bass | C2 (36) | mono, 2.2 s |
| `bell.flac` | an FM bell | C5 (72) | mono, 3 s |
| `voice.flac` | a formant "a" with a delayed vibrato | C4 (60) | mono, 4 s |
| `break.flac` | two bars of drums at 100 BPM, made of Marram's kick, snare and hats | | mono, 4.8 s |

Their names and sounds are permanent identifiers, as parameter keys are
(`plugins/AGENTS.md`): a saved project names them. The script player has
no factory files: there the default shows as missing and a `factory:`
path is refused; the built plugin plays them.

**Factory patches** (each with its factory sample): Tine Keys, Air Pad,
Ping-Pong Pad, Reverse Swell, Harp, Glide Bass, Chime, Chopped Break
(transients, mono), Gated Break (a 16-slice grid, gate), Break Thru, Vowel
Chop and First Hit (one-shot). On `dsp.test`'s notes signal (a note, then
a chord, velocity 0.8) the pitched ones peak at −5.2 (Chime) to
−17.8 dBFS (Glide Bass) (2026-10-29); the slice patches' slices are on
keys 36 up, below that signal's notes, so they are measured with notes of
their own.

## 5. The editor

The script's own, 1240 by 940 design units, drawn and edited in the script
(`editor()`, `draw()`, `pointer()`) in the stock look; the audio code sends
the waveform's overview, the region, loop and slices as they play, each
voice's place, the close-ups, the output peaks and the newest note's
envelopes and LFOs as `Shared` members.

- **Header**: the name and a coppice stool, a shoot a voice, lit while it
  sounds; a line saying what it plays; the mode; the four macros; the
  output; voices sounding of the limit, and the meter.
- **Sample**: the file (`e.file()`: a click chooses, a drop loads), Reverse
  and Normalize; the waveform of the whole file, the region outside start
  and end dimmed, with a flag for each (drag; Shift finely; double click
  resets), the loop as a bracket (drag an end, or the middle to move it
  whole) with its crossfade, or the slices numbered with their keys; a line
  where each voice plays. Under it the start, end and gain, then the
  mode's own controls, and two close-ups, 2.5 ms either side: the start (in
  slice, the newest note's slice), and the loop's seam as it plays (or the
  end). They run as the sound plays: reversed, the start is the region's
  end, and they say so.
- **Pitch & Voice**, **Filter** (its response, a node to drag: cutoff
  across, resonance up), **Envelopes** (amp, Env 2), **LFOs** (two) and
  **Modulation** (`e.routes()`), as Marram's.
- **Keyboard**: C1 to C7, playing the plugin; over it a mark on the root
  key (classic, one-shot) or a bar on each slice's key, the newest note's
  lit (slice).

Controls that do not apply are dimmed in place, so nothing moves when a
mode or type changes. Every dial drags (Shift: finely), turns by the wheel
and resets on a double click.

## 6. Performance

Release build, one core, 48 kHz, `clap-bench` (2026-10-29): 0.18 % of a core
for the default (the keys, filter off, normal quality; 1.99 refs), 1.29 %
for Air Pad (a stereo pad through the ladder), the factory patches 5.28
refs on average. Marram the same way: 0.53 % (5.94 refs), its factory
patches 10.2. The release `Coppice.clap` is 3,364,040 bytes, the factory
samples 918,861 of them.
