# Making a plugin with a DSP script

A DSP script is a plugin you write: one C++ file, saved to the script
library, compiled at once, and from then on an ordinary plugin to Quire.
You load it with `fx_add`, set it in physical units with `fx_patch_merge`
and `fx_param_set`, and save it with the project, exactly like a stock
plugin. The language: `guides/dsp-language.md` (read it first; it is short).

## The loop

1. **Start from the closest example** (`generated/dsp-examples.md`: a
   gain, a tilt EQ, a sidechain compressor, an echo, an oversampled drive,
   a spectral effect, a convolution reverb, a synth, a sampler, a meter
   with an editor of its own) rather than
   from nothing, and decide the parameters first: what a person would
   turn, in which unit, over what range.
2. **Write it**: `dsp_write {name: "tilt_eq", source: "<the whole file>"}`.
   - `ok: false`: fix the `diagnostics` in order, each at its `line` and
     `column`. The `hint` is the fix; apply it rather than guessing. Fix the
     first one first, since later ones often follow from it. Write again.
   - `ok: true`: `plugin` gives its `id`, its `kind` (`effect` or
     `instrument`) and its parameters as the patch will show them (`key`,
     `unit`, range, default), and its `files` when it has any (a sample, an
     impulse response). `listed: true` means `plugin_list` shows it now.
   - `dsp_check {source}` compiles without saving, for trying a fragment.
3. **Load it** on a track: `fx_add {track, plugin: "<its id>", patch:
   {params: {...}}}`. It answers `semantic: true`: `fx_describe`,
   `fx_patch_get`, `fx_patch_merge` and `fx_param_set` work in its units.
   An instrument goes first on its track, and plays the track's notes:
   `item_add {track, bar, notation: "..."}` (`guides/notation.md`).
   `fx_patch_list {fx}` lists its read-only factory patches as canonical
   patch documents; `fx_patch_set {fx, name: "Warm"}` applies one as a
   complete replacement, undoable like any patch edit. Give exactly one of
   `name` or `patch` to `fx_patch_set`. In the sandbox, use
   `preset_list {plugin: "<id>", source: "factory"}` and
   `preset_load {instance, name: "Warm", source: "factory"}`; listing marks
   them `read_only: true`. User presets remain the default source and may
   share a name with a factory patch.
4. **Measure it.** `dsp_test {script: "tilt_eq"}` plays a
   sine, a sweep, an impulse, pink noise and silence through a fresh
   instance and reports levels, the sine's `gain_db` and `thd_percent`, the
   impulse's `response_db` at octave centres, and `problems` in words
   (clipping, DC, sound from silence, no output, NaN, a loop that never
   ended). An instrument gets notes instead: a note and a chord (`notes`),
   A4 alone (`note`, with the `pitch_hz` heard) and no notes (`silence`),
   with `release_ms`, how long it sounds after the last release; its
   problems include a note out of tune and sound that never dies away (a
   voice never released or never finished). Run it again with `patch: {params: {...}}` at the extremes of
   each parameter. A script with files plays nothing from them until it
   has them: give each an absolute path, `patch: {properties: {room:
   "/path/hall.wav"}}`, here and in `fx_add`/`fx_patch_merge`; a path that
   names no readable WAV, AIFF or FLAC file is refused, saying why. `problems` should be empty unless the plugin means it
   (a distortion clips on purpose). `save: true` keeps the outputs for
   `analyze` and `compare`. On real material, `analyze_track` measures the
   track before and after.
5. **Look at it.** `dsp_preview {script: "tilt_eq"}` plays a test signal
   (notes for an instrument) through a fresh instance and draws its editor
   to a PNG (`path`): read the file to see the controls, meters and the
   script's own drawing as they would look. Every plugin has the automatic
   editor; a script draws its own with `editor()` and `draw()`, and makes
   the drawing respond to the mouse with `pointer()`
   (`guides/dsp-language.md`, The editor). Preview again after changing
   it, and with `patch` to see other settings. `pointer` acts first, in the
   picture's pixels at scale 1: `[{action: "drag", x: 240, y: 132, to_x:
   330, to_y: 60}]` (also `move`, `press`, `release`, `click`,
   `double_click`, `wheel` with `steps`); `edits` is the merge patch of
   what it changed, so a check that a drag edits what it should needs no
   mouse. `problems` names a loop or an index that went wrong in the
   editor's code, with the line.
6. **Change it** by writing the same name again. Instances already playing
   it switch to the new code within half a second, crossfaded, keeping
   their parameter values. Changing the parameters themselves (adding,
   removing, renaming, a new range) restarts the plugin in the host;
   values carry over by key. A version that does not compile leaves the
   previous one playing, so a mistake never silences the track. An open
   editor is made again from the new code.
   If a track sounds wrong or goes silent, inspect its **live instance**:
   `fx_describe {fx}` in the DAW, `plugin_describe {instance}` in the sandbox.
   `status_available: true` adds structured `problems` with `code`, `message`,
   `scope`, `line` when known, `hint` and `silent`. Follow the hint: a loop
   budget stops the audio; an array index outside its bounds reads zero and
   drops writes; a failed saved edit keeps the older code loaded. Fix and
   save again, then inspect again. Editor problems have `scope: "editor"`
   and do not mean the audio is silent. `status_available: false` means no
   report is available (the DAW needs a live instance), not that it is healthy.
   `dsp_test` measures a fresh instance, so it cannot diagnose the existing
   instance's stopped state.
7. **Tell the human** what it does, which parameters matter, and which
   track to listen to. Taste is theirs.
8. **Export it** when they want it in another DAW. `dsp_export {script:
   "tilt_eq"}` writes `<Name>.clap` (in the script's folder under
   `export/`, or `folder: "/home/<them>/.clap"` to install it) and the
   notices to ship beside it: ready now, for this platform. `format:
   "source"` writes a C++ project instead, which builds the same plugin
   (same id, parameters and state) as CLAP, VST3 and AU anywhere; its
   README says how, and `license` (`MIT` unless `GPL-3.0-or-later`) is
   theirs to choose. Either plays the script as it was when exported;
   export again after changing it. Projects here keep using the library's
   version, under the same id.

`dsp_list` shows every script in the library (and whether it compiles);
`dsp_get {name}` returns one's source. The DAW and the sandbox share the
library and have the same seven commands.

## Test what changes over time

A static patch cannot show whether moving a knob clicks or becomes unstable.
Use `automation` to send discrete changes during every selected signal, in
physical units and at sample offsets rounded from seconds:

```json
{"script":"tilt_eq","signals":["sine"],"automation":[
  {"param":"output","at_s":0.253,"value":-12},
  {"param":"output","at_s":0.753,"value":0}
]}
```

Use the plugin's patch keys and ranges; enum keys and booleans work too.
There is no automatic ramp: supply intermediate points for gradual motion
or a dense staircase to probe zipper noise (up to 1024 points). Each signal
gets a fresh instance/initial patch. `automation.max_step` is the largest
adjacent-sample change, `baseline_max_step` the same render without changes,
`excess_step` their positive difference, all in full-scale amplitude. A step
over 0.05 and four times baseline suggests checking `.smoothed()` and
`.at(i)`. Intended changes can also increase it, and a small step does not
prove inaudibility. Read `problems` for clipping, nonfinite output and loops
from both passes, then save and inspect/listen to the primary audio.

For a sidechain compressor, send a key while the main sine plays:

```json
{"script":"compressor","signals":["sine"],"level_db":-30,
 "sidechain":{"signal":"sine","level_db":-6,"frequency_hz":100,
              "start_s":0.25,"length_s":0.5}}
```

Only a plugin with a key input accepts it. The key is connected and silent
outside its span. All those key fields are optional: the example's values
are defaults for `seconds: 1`; default start/length scale with `seconds`.
`sidechain.gain_change_db` compares output in the middle half of the span
against a fresh render with a connected silent key and the same automation:
a negative value measures ducking. `rms_dbfs`/`reference_rms_dbfs` give the
levels. Gain change is null if either output is below -120 dBFS (e.g. main
silence); slow attack may need a longer key/longer `seconds` to settle.
Repeat with `sidechain: {signal: "silence"}` as a control.

For an instrument, use a note sequence to check velocity, overlapping
notes, repeated keys and release:

```json
{"script":"synth","notes":[
  {"key":60,"velocity":0.8,"start_s":0.013,"length_s":0.2},
  {"key":67,"velocity":0.6,"start_s":0.303,"length_s":0.4}
]}
```

Keys are 0..127, velocity (0,1] (default 0.8); spans must fit within
`seconds` (the final release may be exactly at its end). Distinct note IDs
let repeated keys overlap. With custom notes the default signals are
`notes` and `silence`; don't select the A4 `note` signal or only silence.
`release_ms` starts at the actual final note-off, and no A4 tuning check is
reported for a custom sequence. Without `notes`, the usual A4 check remains.
At the same sample, parameter changes precede note-offs, then note-ons.

Automation and a key may be combined. THD/impulse response are omitted with
automation or a non-silent key because they no longer describe static
behavior; sine gain is measured across the changing render. Only primary
outputs are saved; `cpu_percent` includes reference renders too.

## Follow the stock plugins

Scripted plugins are controlled by agents through their patches, so they
follow the same conventions as the stock plugins:

- **Physical units and real ranges**: a cutoff in Hz from 20 to 20000, a
  time in ms, a level in dB, a mix in percent. Never a bare 0..1 knob for
  something that has a unit.
- **Names a person would say** ("Cutoff", "Feedback"), one `.help()`
  sentence each, `.in()` groups when there are more than five or six.
- **Defaults that do something sensible**, and a neutral setting reachable
  (0 dB, 0% wet, a flat tilt).
- **Useful factory patches** in `factoryPatches(Patches& p)`: a neutral
  reference, a subtle everyday setting, and a stronger useful contrast.
  Name the sound or use ("Warm", "Slap", "Parallel crunch"); balance output
  levels so louder does not win by accident. For a percent write 30, as in
  the patch. Start with the tilt, echo and drive examples. Measure each
  patch with `dsp_test {script, patch: <the listed patch>}` and listen on
  suitable material. They also appear in other hosts' factory discovery
  and the preset bar. Editing factory declarations refreshes the immutable
  preset table through a plugin restart; ordinary DSP-only edits still swap
  in place.
- **`.smoothed(20)`** on anything that scales the audio directly (levels,
  mix, pan), read with `.at(i)`, so automation and edits never click.
- **A stable id**: `Info.id` is how projects find the plugin. Pick it once
  (`com.<you>.<name>`) and keep it. Renaming a parameter's member changes
  its key: add `.withKey("old_key")` when a project already uses it.

## Mistakes to avoid

- A percent parameter reads as a fraction: `percent("Mix", 30)` is `0.3` in
  code, `30` in the patch.
- `io.channels` may be 1 (mono) or 2: index per-channel state with `c` and
  loop `c < io.channels`; an `Array<Biquad, 2>` covers both.
- Allocation belongs in `prepare()`: `DelayLine::prepare`, `Buffer::resize`.
  Size delay lines from `sampleRate()` there, with a little headroom.
- Feedback must stay below 1 (cap a feedback parameter at 95%), and a
  filter's resonance in range, or the output grows without bound.
- Coefficients belong in `update()`, which runs when parameters change; a
  smoothed parameter that drives a filter needs its coefficients set per
  sample (or per few samples) in `process()`.
- Integer `/` by a variable is refused: `idiv(a, b)`, or keep it in double.

## An echo, for a start

```cpp
#include <supple/script.h>

// A feedback delay: what comes back is fed in again, quieter each time.
struct Echo : Effect {
    static constexpr Info info = {.name = "Echo", .id = "com.example.echo", .tail = -1};

    Param time = milliseconds("Time", 1, 2000, 350).help("Time between repeats.");
    Param feedback = percent("Feedback", 40, 0, 95).help("How much of each repeat returns.");
    Param mix = percent("Mix", 30).smoothed(20).help("Dry to wet.");

    Array<DelayLine, 2> lines;

    void prepare() {
        for (auto& line : lines) line.prepare(toInt(sampleRate() * 2.0) + 4);
    }

    void process(Block& io) {
        const double delay = time * 0.001 * sampleRate();
        for (int c = 0; c < io.channels && c < 2; ++c) {
            DelayLine& line = lines[c];
            for (int i = 0; i < io.frames; ++i) {
                const double dry = io[c][i];
                const double wet = line.read(delay);
                line.push(dry + wet * feedback);
                io[c][i] = lerp(dry, wet, mix.at(i));
            }
        }
    }
};
```
