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
   `fx_patch_list {fx}` lists its factory patches and `fx_patch_set {fx,
   name: "Warm"}` applies one (undoable). In the sandbox: `preset_list
   {plugin, source: "factory"}` and `preset_load {instance, name, source:
   "factory"}`.
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

`dsp_test` can also move parameters, feed a sidechain and play your own
notes. Each signal still gets a fresh instance.

- `automation: [{param, at_s, value}]`: changes in the patch's units at
  those times, during every signal (no ramps: give several points for a
  gradual move). `automation.excess_step` is how much larger the biggest
  sample-to-sample step is than without the automation; when `problems`
  says it may click, make the parameter `.smoothed()` and read it with
  `.at(i)`.
- `sidechain: {signal, level_db, frequency_hz, start_s, length_s}`: a key
  for a plugin with a sidechain input, playing over part of the render.
  `sidechain.gain_change_db` is how far the output moved while the key
  played, compared with a silent key: negative is ducking. Without
  `sidechain`, nothing is routed to the key (`io.sidechainConnected` is
  false).
- `notes: [{key, velocity, start_s, length_s}]`: an instrument's notes
  (MIDI keys) in place of the fixed note and chord; `release_ms` counts
  from the last note-off.

```json
{"script": "compressor", "signals": ["sine"], "level_db": -30,
 "sidechain": {"signal": "sine", "level_db": -6, "start_s": 0.25, "length_s": 0.5}}
```

With automation or a sounding key, THD and the impulse response are left
out: they describe a plugin that holds still.

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
- **Useful factory patches** (`factoryPatches`, as in the tilt, echo and
  drive examples): a neutral one, an everyday setting and a stronger
  contrast, named for the sound or the use ("Warm", "Slap", "Parallel
  crunch"), with levels balanced so louder does not win. Check each with
  `dsp_test {script, patch}`. Other hosts show them in their preset
  browsers, and the plugin in its preset bar.
- **`.smoothed(20)`** on anything that scales the audio directly (levels,
  mix, pan), read with `.at(i)`, so automation and edits never click.
- **A stable id**: `Info.id` is how projects find the plugin. Pick it once
  (`com.<you>.<name>`) and keep it. Renaming a parameter's member changes
  its key: add `.withKey("old_key")` when a project already uses it.

## Mistakes to avoid

- A percent parameter reads as a fraction: `percent("Mix", 30)` is `0.3` in
  code, `30` in the patch and in `factoryPatches`.
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
