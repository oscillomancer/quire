# Making a plugin with a DSP script

A DSP script is a plugin you write: one C++ file, saved to the script
library, compiled at once, and from then on an ordinary plugin to Quire.
You load it with `fx_add`, set it in physical units with `fx_patch_merge`
and `fx_param_set`, and save it with the project, exactly like a stock
plugin. The language: `guides/dsp-language.md` (read it first; it is short).

## The loop

1. **Start from the closest example** in the language guide (the tilt EQ,
   the synth for an instrument, or the echo below) rather than from
   nothing, and decide the parameters first: what a person would turn, in
   which unit, over what range.
2. **Write it**: `dsp_write {name: "tilt_eq", source: "<the whole file>"}`.
   - `ok: false`: fix the `diagnostics` in order, each at its `line` and
     `column`. The `hint` is the fix; apply it rather than guessing. Fix the
     first one first, since later ones often follow from it. Write again.
   - `ok: true`: `plugin` gives its `id`, its `kind` (`effect` or
     `instrument`) and its parameters as the patch will show them (`key`,
     `unit`, range, default). `listed: true` means `plugin_list` shows it
     now.
   - `dsp_check {source}` compiles without saving, for trying a fragment.
3. **Load it** on a track: `fx_add {track, plugin: "<its id>", patch:
   {params: {...}}}`. It answers `semantic: true`: `fx_describe`,
   `fx_patch_get`, `fx_patch_merge` and `fx_param_set` work in its units.
   An instrument goes first on its track, and plays the track's notes:
   `item_add {track, bar, notation: "..."}` (`guides/notation.md`).
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
   each parameter. `problems` should be empty unless the plugin means it
   (a distortion clips on purpose). `save: true` keeps the outputs for
   `analyze` and `compare`. On real material, `analyze_track` measures the
   track before and after.
5. **Change it** by writing the same name again. Instances already playing
   it switch to the new code within half a second, crossfaded, keeping
   their parameter values. Changing the parameters themselves (adding,
   removing, renaming, a new range) restarts the plugin in the host;
   values carry over by key. A version that does not compile leaves the
   previous one playing, so a mistake never silences the track.
6. **Tell the human** what it does, which parameters matter, and which
   track to listen to. Taste is theirs.
7. **Export it** when they want it in another DAW. `dsp_export {script:
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
library and have the same six commands.

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
