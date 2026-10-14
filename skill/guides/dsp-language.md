# DSP scripts: the language

The canonical reference for DSP scripts (ADR-0032): plugins written as one
C++ file, compiled when they are saved and run by the player plugin. The
same file also compiles as ordinary C++ against `<supple/script.h>`, which
is how a script becomes a standalone plugin, and it means the same thing
both ways. Workflow and commands: `guides/dsp-workflow.md`.

## A script

```cpp
#include <supple/script.h>

// A tilt equaliser: highs up and lows down around a pivot, or the reverse.
struct Tilt : Effect {
    static constexpr Info info = {.name = "Tilt", .id = "com.example.tilt", .vendor = "Example"};

    Param tilt = decibels("Tilt", -6, 6, 0).help("Positive brightens: highs up, lows down.");
    Param pivot = frequency("Pivot", 200, 5000, 1000).help("Where the two shelves meet.");
    Param output = decibels("Output", -12, 12, 0).smoothed(20);

    Array<Biquad, 2> low, high;

    void update() {
        for (int c = 0; c < 2; ++c) {
            low[c].lowShelf(pivot, 0.707, -tilt * 0.5);
            high[c].highShelf(pivot, 0.707, tilt * 0.5);
        }
    }

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) {
                double x = high[c].process(low[c].process(io[c][i]));
                io[c][i] = x * dbToGain(output.at(i));
            }
    }
};
```

- The first line is `#include <supple/script.h>`, the only include there is.
- One struct derives from `Effect` (audio in, audio out) or `Instrument`
  (notes in, audio out; see Instruments below): the plugin. Others are
  plain structs it holds as members. `NoteEffect` is refused: scripts send
  no notes yet.
- `static constexpr Info info` is its identity; `Param` members are its
  parameters; `prepare()`, `update()` and `process(Block&)` are the hooks.
- Every member has a starting value (`double phase = 0.0;`), or is an object
  that starts empty (`Biquad`, `DelayLine`, `Array<...>`).

## Identity: `Info`

| Field | Meaning |
|---|---|
| `name` | Shown in hosts and in `plugin.list` |
| `id` | Reverse DNS, `com.example.tilt-eq`: words of letters, digits and `-` joined by `.` (no `_`). Saved sessions find the plugin by it: never change it once the plugin is used |
| `version` | Major, minor, patch: `"1.0.0"` (the default) |
| `vendor`, `description`, `url` | As they say |
| `channels` | Main bus widths offered, the first the default: `{2, 1}` (the default) is stereo, then mono |
| `sidechain` | `true`: a key input as wide as the main bus (`io.sidechain(c)`) |
| `outputs` | Names of extra outputs (`io.output(bus, c)`) |
| `voices` | An instrument's voices, 1 to 64 (8 when not given) |
| `latency` | Samples of delay the plugin adds, reported to the host |
| `tail` | Samples of output after the input stops; `-1` forever (feedback) |

## Parameters

A parameter is a member made from a description:
`Param gain = decibels("Gain", -24, 24, 0);`. The description sets its name,
range, default and unit; Quire, hosts and agents see it in that unit (a
patch says `"gain": -3`), exactly as with the stock plugins.

| Factory | Unit, travel |
|---|---|
| `decibels(name, min, max, def)` | dB, linear |
| `frequency(name, min, max, def)` | Hz, logarithmic |
| `milliseconds(name, min, max, def)` | ms; logarithmic when `min > 0`, else cubic |
| `percent(name, def, min = 0, max = 100)` | given in percent, **read as a fraction** (30 reads 0.3) |
| `ratio(name, min, max, def)` | a ratio (4 is 4:1), logarithmic |
| `semitones`, `cents`, `octaves`, `degrees` `(name, min, max, def)` | as named, linear |
| `integer(name, min, max, def)` | a whole number; read `.index()` |
| `choice(name, {"Low Pass", "High Pass"}, def)` | an option; read `.index()` (0, 1, ...) |
| `toggle(name, def)` | on or off; read `.on()` |
| `continuous(name, min, max, def, Unit::...)` | anything else: `Unit::Bpm`, `Unit::Samples`, or no unit (`Unit::None`, the default) |

Modifiers chain: `decibels("Output", -24, 12, 0).smoothed(20).help("...")`.
Ranges and smoothing times must be finite, power-curve exponents finite and
positive, and integer parameter ranges must fit in `int`.

| Modifier | Effect |
|---|---|
| `.help("one sentence")` | The tooltip and the schema's description. Write one for every parameter |
| `.in("Group")` | A section in the editor and in hosts |
| `.smoothed(ms)` | The framework glides it per sample; read `p.at(i)` in `process` |
| `.offAtMin()` | Its minimum means off ("-inf dB"); `.gain()` is exactly 0 there |
| `.meter()` | An output the plugin reports with `p.set(v)`: read-only, never saved |
| `.modulated()`, `.notAutomatable()` | As named |
| `.polyModulated()` | An instrument's: modulated per note by the host; each voice reads `p.forVoice(v)` |
| `.role({"level"})` | What it does, for agents: `level`, `time`, `brightness` ... |
| `.curved(Curve::Log)`, `.curved(Curve::Pow, k)` | Knob travel |
| `.withKey("key")`, `.withId(n)` | Override the patch key or the CLAP id (see below) |

Reading: a `Param` reads as a `double` in its unit (`gain * 0.5`), with
`.value()`, `.at(i)` (the glide at frame `i`), `.index()`, `.on()`,
`.gain()` (dB as linear gain) and, in `update()`, `.changed()`.

**Factory patches** are declared in the plugin struct, in patch values (a
percent as 30, though the audio code reads it as 0.3):

```cpp
void factoryPatches(Patches& p) {
    p.add("Flat").set(tilt, 0).set(pivot, 1000).set(output, 0);
    p.add("Warm").set(tilt, -3).set(pivot, 800).set(output, 0);
    p.add("Air").set(tilt, 3).set(pivot, 2500).set(output, -1);
}
```

Each patch is `p.add("Name")` and `.set(member, value)` calls with a
constant, a bool or a choice's key (`.set(shape, "soft")`); a parameter left
out takes its default. Nothing else belongs here (no loops or locals), and
the compiler checks names, ranges and choices.

**Keys and IDs.** A parameter's patch key is its member's name in
snake_case (`lowCut` is `low_cut`), and its CLAP id is a hash of the key.
Renaming a member changes both, and sessions that saved the old one lose
that value: give the new name `.withKey("old_key")` to keep it. A member
cannot share its factory's name, as in C++ (`Param ratio = ratio(...)`
does not compile): write `Param slope = ratio("Ratio", 1, 20, 4).withKey("ratio")`.

Follow the stock plugins: physical units with sensible ranges, a name a
person would say ("Cutoff", not "Freq1"), a `.help()` line for each, `.in()`
groups once there are more than five or six, and `.smoothed()` on anything
that scales audio directly (levels, mix) so that moving it does not click.

## Hooks

| Hook | When | What goes there |
|---|---|---|
| `void prepare()` | Before audio starts, and when the sample rate or block size changes. `sampleRate()` and `maxBlockSize()` are known | Allocation (`DelayLine::prepare`, `Buffer::resize`), seeding, anything sized from the sample rate |
| `void update()` | After `prepare()`, then at the start of a block whenever a parameter changed (blocks are split at each change, so automation is sample-accurate) | Turning parameters into coefficients; `p.changed()` says which moved |
| `void process(Block& io)` | Every block | The audio |
| `void editor(Editor& e)`, `void draw(Canvas& g)`, `void pointer(const Pointer& p)` | The editor's (optional) | Its controls, its drawing, and what the pointer does to the drawing: see The editor |

There is no `reset()` to write: the runtime returns every member to what
`prepare()` left, by itself. The script never calls a hook.

`Block`: `io.channels`, `io.frames` (a block may be a single frame),
`io[c][i]` (read the input, write the output, in place), `io.transport`
(`playing`, `tempo`, `beats`, `barStart`, time signature),
`io.sidechain(c)[i]` (the key input with Info's `.sidechain`; silent until
`io.sidechainConnected`, which is false while nothing is routed to the key
in Quire and in hosts that deactivate an unused port; a host that always
feeds the port reports it connected, so a plugin there may want a choice
parameter for its key), `io.output(bus, c)[i]`.
From any member function: `sampleRate()`, `maxBlockSize()`, `channels()`,
`offline()`. In `prepare()`, `setLatency(samples)` reports a delay the
plugin adds that depends on the sample rate (an `Oversampler`'s, a
lookahead), where Info's `.latency` is fixed.

## Instruments

An instrument derives from `Instrument`. The runtime gives each note to one
of its voices (stealing the oldest when all are busy, holding released
notes while the sustain pedal is down), and calls its voice hooks. The
script keeps each voice's sound in its own array, indexed the same way:

```cpp
#include <supple/script.h>

// A polyphonic synth: a saw per voice through a low-pass filter and an
// envelope. With Glide above 0 it plays one note at a time, sliding.
struct Synth : Instrument {
    static constexpr Info info = {.name = "Synth", .id = "com.example.synth", .voices = 8};

    Param attack = milliseconds("Attack", 1, 2000, 5).help("How fast a note rises.");
    Param release = milliseconds("Release", 5, 5000, 300)
                        .help("How long a note fades once its key is up.");
    Param cutoff = frequency("Cutoff", 200, 16000, 3000).help("The low-pass filter's corner.");
    Param glide = milliseconds("Glide", 0, 1000, 0)
                      .help("The slide from note to note; above 0, one note at a time.");
    Param level = decibels("Level", -48, 0, -12).smoothed(20).help("The output level.");

    struct Voice {
        Oscillator osc;
        Adsr env;
        Biquad filter;
    };
    Array<Voice, 8> voice;

    void prepare() {
        for (Voice& v : voice) v.osc.setShape(Oscillator::Shape::Saw);
    }

    void update() {
        for (Voice& v : voice) {
            v.env.set(attack, 200, 0.7, release);
            v.filter.lowpass(cutoff, 0.707);
        }
        setVoiceMode(glide > 0 ? VoiceMode::Legato : VoiceMode::Poly);
        setGlide(glide);
    }

    // Voice v starts a note. retrigger is false when it glides on from the
    // note before (Legato): its envelope keeps going.
    void startVoice(int v, bool retrigger) {
        if (retrigger) voice[v].env.gate(true);
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }

    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            Voice& s = voice[v];
            const double velocity = voiceVelocity(v);
            for (int i = 0; i < io.frames; ++i) {
                s.osc.setFrequency(voiceHz(v, i));
                const double tone = s.filter.process(s.osc.next());
                const double x = tone * s.env.next() * velocity * dbToGain(level.at(i));
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!s.env.active()) voiceFinished(v);
        }
    }
};
```

| Hook | When |
|---|---|
| `void startVoice(int v)` | Voice `v` starts a note. Or `startVoice(int v, bool retrigger)`: `retrigger` is false in Legato mode for a note that glides on from the one sounding |
| `void releaseVoice(int v)` | Its key is up (and no pedal holds it): start the release |
| `void stopVoice(int v)` | (optional) Silence it at once: stolen for a new note, or choked |

`process()` receives `io` silent and adds every voice into it. Once a
voice's release has died away it calls `voiceFinished(v)`, which tells the
host that note is over: a voice never finished keeps its note open forever.

| Method | Gives |
|---|---|
| `voiceCount()` | Info's `.voices` |
| `voicePlaying(v)` | From `startVoice` until `voiceFinished` |
| `voiceKey(v)`, `voiceChannel(v)`, `voiceVelocity(v)` | The note: MIDI key, channel, velocity 0..1 |
| `voicePitch(v, i)`, `voiceHz(v, i)` | At frame `i`: semitones (key, glide and pitch bend), and Hz |
| `voiceVolume(v)`, `voicePan(v)` | The note's expressions as the host sends them: a gain 0..4 (1 until sent), 0..1 (0.5, the centre) |
| `voiceVibrato(v)`, `voiceExpression(v)`, `voiceBrightness(v)`, `voicePressure(v)` | 0..1, 0 until sent (MPE arrives as these; a tuning expression is in `voicePitch`) |
| `setVoiceMode(VoiceMode::Poly)` | Or `Mono` (the newest key held, returning to the last), `Legato` (Mono, gliding without restarting) |
| `setGlide(ms)`, `setBendRange(semitones)` | The slide between notes (Mono, Legato); the bend's reach (2) |

## Files

A sampler's sample or a reverb's room is a file the user (or an agent)
picks, declared as a member of the plugin struct like a parameter:

```cpp
AudioFile sample = audioFile("Sample").help("The sound every key plays.");
Convolver room = impulseResponse("Room").help("A recording of a space's echo.");
```

Its path is a property of the plugin, saved with it and set in a patch,
`{"properties": {"sample": "/home/me/kick.wav"}}` (the key is the member's
name in snake_case, or `.withKey("...")`). File keys use lowercase letters,
digits and underscores, without dots. The runtime loads WAV, AIFF or FLAC
off the audio thread, resamples to the session's rate, and hands
it over at the start of a block; until then, and with no file, `loaded()`
is false. Not in `prepare()`: files arrive after it.

| Object | Use |
|---|---|
| `AudioFile` | `loaded()`, `channels()`, `frames()`; `sample(c, i)` (0 outside; a channel past the file's last reads its last), `read(c, position)` between samples (cubic), for playing at another speed |
| `Convolver` | `process(c, x)`: channel `c` through the impulse response (its first 20 s), no latency; 0 until loaded. `loaded()`, `length()` |

Files are members of the plugin struct only, one per member (no arrays),
and never assigned: read them, or keep what you need in members of your
own.

## The editor

With no editor code a plugin gets the automatic editor: every parameter, by
group (`.in()`), with meters. To draw its own, it adds `editor()`, `draw()`
or both, and `pointer()` to make its drawing respond to the mouse:

| Hook | When | What goes there |
|---|---|---|
| `void editor(Editor& e)` | Once, as the editor opens (and after a reload) | Its size and its controls |
| `void draw(Canvas& g)` | At each repaint | What lies under the controls: panels, graphs, meters, text |
| `void pointer(const Pointer& p)` | As the pointer acts where no control is (needs `draw()`) | Handles to drag, shapes to click: edits parameters with `Param::edit()` |

```cpp
#include <supple/script.h>

struct Level : Effect {
    static constexpr Info info = {.name = "Level", .id = "com.example.level"};

    Param gain = decibels("Gain", -24, 24, 0).smoothed(20).help("The level out.");
    Shared<double> peak;  // what the editor shows, sent by the audio code
    double held = 0.0;    // the audio code's own

    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i)
            for (int c = 0; c < io.channels; ++c) {
                io[c][i] *= dbToGain(gain.at(i));
                held = max(abs(io[c][i]), held * 0.9999);
            }
        peak.set(gainToDb(held));
    }
    void editor(Editor& e) {
        e.size(240, 180);
        e.knob(gain, 20, 50, 88, 100);
    }
    void draw(Canvas& g) {
        g.panel(8, 8, 224, 164, "Level");
        g.meter(peak.get(), 150, 50, 20, 100);
        g.fillStyle(Theme::textMuted);
        g.monoFont(11);
        g.fillText(format("%.1f dB", peak.get()), 140, 166);
    }
};
```

Editor code runs on the host's main thread, in a copy of the plugin of its
own. There each `Param` reads the value being heard (with the host's
modulation; a `.meter()` what the plugin reports) and each `Shared` member
what the audio code last sent; any other member it uses is the editor's own
(a zoom, a hover). A member both editor code and `prepare()` or `process()`
use is refused, so the audio code shares what the editor shows:

| Member | Audio code | Editor code |
|---|---|---|
| `Shared<double> level;` (or `float`, `int`, `bool`) | `level.set(x)` | `level.get()` |
| `SharedArray<float, 256> scope;` (an `Array`) | `scope[i] = x` | `scope[i]` |

They are members of the plugin struct, start at zero, and arrive together,
as of one moment, about 120 times a second while the editor is open. Editor
code never changes them, and changes a parameter only as a user would:
through a control, or by `edit()` in `pointer()`.

Controls, `e.knob(p, x, y, w, h)` and the like, are the framework's own,
bound to a parameter and labelled with its name, in design units (the
window scales everything): `knob`, `slider` (along its longer side),
`toggle`, `choice` (buttons for up to four options, a menu for more),
`readout` (shown, not edited: a `.meter()`), `xyPad(across, up, ...)`,
`envelope(attack, decay, sustain, release, ...)`, `file(member, ...)` (an
`AudioFile` or `Convolver`: its name; a click chooses, a dropped file is
taken), `keyboard(lowKey, highKey, ...)` (an instrument's) and
`meters(...)` (the input and output peak meters). `e.size(w, h)`: 480 by
320 unless given.

`Canvas` is HTML canvas's 2D context, as methods:

| Calls | |
|---|---|
| `fillStyle(c)` `strokeStyle(c)` `globalAlpha(a)` `lineWidth(w)` `lineCap(LineCap::Round)` `lineJoin(LineJoin::Round)` `linearGradient(x0, y0, x1, y1, from, to)` `radialGradient(x, y, r0, r1, from, to)` | Styles, until changed or `restore()`d. A colour is `0xRRGGBB` or one of the palette's: `Theme::ground` `surface` `raised` `recess` `line` `lineLit` `textFaint` `textMuted` `text` `accent` `accent2` `modulation` `alert`. Fill and stroke start as `Theme::text` |
| `fillRect` `strokeRect` `clearRect` `(x, y, w, h)` | Each a path of its own |
| `beginPath()` `moveTo` `lineTo` `bezierCurveTo` `quadraticCurveTo` `arc(x, y, r, start, end)` `arcTo` `rect` `roundRect(x, y, w, h, r)` `ellipse(x, y, rx, ry)` `circle(x, y, r)` `closePath()`, then `fill()` or `stroke()` | Paths. Angles in radians, 0 to the right, clockwise |
| `save()` `restore()` `translate` `rotate` `scale` `clip(x, y, w, h)` | Transforms and a rectangular clip |
| `font(size)` `boldFont(size)` `monoFont(size)` `textAlign(TextAlign::Center)` `textBaseline(TextBaseline::Middle)` `fillText(text, x, y)` `measureText(text)` | Text (`monoFont` for numbers) |
| `panel(x, y, w, h, "Title")` `well` `meter(levelDb, ...)` `frequencyGrid(x, y, w, h, lowHz, highHz)` `levelGrid(x, y, w, h, lowDb, highDb, stepDb)` | The editor's furniture, in its look |
| `width()` `height()` | The editor's size |

Text is a string literal, `format("%.1f dB", x)` (printf's number
conversions, up to four values), or a parameter's `p.text()` (its value as
hosts show it) and `p.name()`; it lasts until the hook returns, so a local
`const char*` holds it but a member cannot. `hzToX(hz, lowHz, highHz, x, w)`
and `dbToY(db, lowDb, highDb, y, h)` place a graph's points on the grids'
lines. To draw a filter's response, give the editor a `Biquad` of its own,
designed from the parameters in `draw()`, and plot its `responseDb(hz)`.
Editor code may loop freely within a generous budget; a hook that spends it
stops, and the editor says where. `dsp.preview` saves the editor as a PNG.

`pointer(p)` hears what the mouse does where no control is, and the editor
repaints after each call. `p.action()` is what happened:

| `PointerAction::` | |
|---|---|
| `Press` | The main button went down here (`p.doubleClick()`: the second of two). The pointer is the script's until the release |
| `Drag` | It moved with that button down, wherever it went: `p.dx()`, `p.dy()` since the last |
| `Release` | The button came up (or the editor lost focus mid-drag) |
| `Move` | It moved with the button up: a hover, over controls too |
| `Wheel` | The wheel turned here: `p.wheel()` notches, positive up |
| `Leave` | It left the editor |

`p.x()`, `p.y()` are where, in design units; `p.over(x, y, w, h)` and
`p.within(x, y, radius)` test it; `p.shift()` (fine adjustment, by custom),
`p.ctrl()` and `p.alt()` are the keys held. `xToHz(x, lowHz, highHz, gx, gw)`
and `yToDb(y, lowDb, highDb, gy, gh)` undo `hzToX` and `dbToY`. A parameter
changes as a user turning its knob would change it: `p.edit(value)` (in its
units, kept in range), `p.editNormalized(position)` (0 to 1, as its knob
travels; `p.normalized()` reads it) or `p.reset()` (its default). The host
records it, the audio code hears it, and `value()` has it at once. A
press's edits, through its drags to its release, are one gesture, however
many parameters they change; a wheel's are one per burst, and
edits on a `Move` or a `Leave` do nothing. Only `pointer()` (and what it
calls) may edit; `draw()` runs with no user acting. A hover or a drag
state is a member of the editor's own:

```cpp
    bool held = false;  // the editor's own

    void pointer(const Pointer& p) {
        if (p.action() == PointerAction::Press) held = p.within(nodeX(), nodeY(), 10);
        if (p.action() == PointerAction::Drag && held) {
            cutoff.edit(xToHz(p.x(), 20, 20000, 20, 440));
            resonance.edit(resonance - p.dy() * (p.shift() ? 0.002 : 0.02));
        }
        if (p.action() == PointerAction::Release) held = false;
    }
```

The `bell` example drags an EQ band's node. `dsp.preview` takes pointer
actions (a click, a drag, a turn of the wheel) before it draws, and says
what they edited, so a script's `pointer()` can be checked without a
mouse.

## Types and statements

- Numbers: `double` (use it by default), `float`, `int`, `bool`. Integer
  arithmetic wraps; float arithmetic stays float, mixed promotes to double.
- `enum class Mode { A, B };`, named `Mode::A`.
- `struct`s with members, default member values and member functions
  (`const` ones too). Free functions. `constexpr` globals and
  `static constexpr` members for constants.
- `Array<T, N>`: fixed size, any element type, `= {1.0, 0.5}`, `.size()`,
  `.fill(v)`, range-based `for`. `Buffer<T>`: numbers, sized by
  `resize(n)` in `prepare()`, a member only.
- Locals have a value when declared: `double y = 0.0;`, `auto& v = voices[i];`.
  References bind to the selected variable or member; a `const` reference
  to a converted number holds a temporary value. Assignments evaluate the
  right-hand value before selecting the target, as in C++20.
  A function's local storage, including temporaries, is limited to 64 KiB;
  keep larger arrays and structs as plugin members.
- `if`/`else`, `switch`, `for`, range-based `for`, `while`, `do`, `break`,
  `continue`, `return`, `? :`.
- Conversions: `static_cast<float>(x)` between numbers; float to int only
  through `toInt` (toward zero), `floorToInt`, `ceilToInt`, `roundToInt`,
  which are defined for every input.

Not in the subset, each refused with a hint: pointers and `nullptr`, C arrays
(`Array` instead), `new`/`delete`, `std::` and other includes (the runtime has
its own `sin`, `min`, `clamp`...), templates, lambdas, `virtual`, inheritance
other than the plugin's, constructors and destructors, `class` (write
`struct`), operator overloading, exceptions, `goto`, recursion, static locals,
unions, `typedef`/`using`, macros and the preprocessor, `sizeof`, namespaces,
`if` with an initializer, `if constexpr`, the comma operator (except in a
`for`'s step), character literals, `constexpr` functions.

## Defined behaviour

A script cannot do what C++ leaves undefined. The compiler refuses:

- integer `/` or `%` by anything but a positive constant: use `idiv(a, b)`
  and `imod(a, b)` (0 when `b` is 0), or `wrap(i, n)` for an index;
- shifts by anything but a constant 0 to 31;
- a float stored in an int without one of the conversions above;
- two operands of one expression that both change the same thing
  (`r.next() - r.next()` on one `Random`);
- a variable or member without a starting value.

At run time: an index outside an `Array`, `Buffer` or channel reads 0 and a
write to it goes nowhere, and the host's log names the line. Each hook has a
budget of loop iterations (65,536 per frame in `process`); a script that
spends it is stopped and silent until it is reset or reloaded, and the log names the
line.

Inspect a loaded instance with `fx_describe` (DAW) or `plugin_describe`
(sandbox): its `problems` report loop budgets, invalid indices and failed
reloads with a source line when known and a corrective hint. `silent: true`
means that problem stopped the audio. Audio observations clear on reset,
activation or adoption of replacement code; reads never clear them.

## Real time

`process()`, and everything it calls, never allocates: `DelayLine::prepare`
and `Buffer::resize` belong in `prepare()`, and the compiler says so
anywhere else. There is no network or clock access, and files come only
through `AudioFile` and `Convolver` members.

## The runtime library

Math (all in double; `min`, `max`, `clamp`, `abs` also take ints): `sin`
`cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `exp` `exp2`
`log` `log2` `log10` `pow` `sqrt` `cbrt` `floor` `ceil` `trunc` `round`
`fmod` `hypot` `abs` `min` `max` `clamp` `lerp(a, b, t)` `sign` `isFinite`,
and the constants `pi`, `twoPi`.

Conversions: `dbToGain(db)`, `gainToDb(g)` (floored at -120), `mtof(note)`,
`ftom(hz)`, `toInt` `floorToInt` `ceilToInt` `roundToInt`, `idiv` `imod`
`wrap`. `compressorGainDb(levelDb, thresholdDb, ratio, kneeDb)` is a
compressor's curve: the gain change in dB (0 or less) for a level, with a
soft knee `kneeDb` wide (0 is hard).

DSP objects (each starts silent; `clear()` returns it there):

| Object | Use |
|---|---|
| `Biquad` | `lowpass` `highpass` `bandpass` `notch` `allpass` `(hz, q)`, `peak` `lowShelf` `highShelf` `(hz, q, gainDb)`; `process(x)`; `responseDb(hz)` |
| `OnePole` | `setCutoff(hz)`; `lowpass(x)`, `highpass(x)` |
| `EnvelopeFollower` | A compressor's detector: `set(attackMs, releaseMs)`; `process(x)` gives the level (linear, rectified); `level()` |
| `DelayLine` | `prepare(maxSamples)` in `prepare()`; `push(x)` once per frame; `read(delaySamples)` (fractional, 0 is the sample just pushed) |
| `Oscillator` | `setShape(Oscillator::Shape::Saw)` (Sine, Saw, Square, Triangle, anti-aliased), `setFrequency(hz)`, `setPulseWidth`, `setPhase`, `next()` |
| `Adsr` | `set(attackMs, decayMs, sustain, releaseMs)`, `gate(on)`, `next()`, `active()` |
| `Oversampler` | For anything nonlinear, one per channel: `prepare(factor)` (2, 4, 8, 16) in `prepare()`; per sample `up(x)`, then `set(k, f(sample(k)))` for `k < factor()`, then `down()`; `latency()` (report it with `setLatency`) |
| `Stft` | Spectral effects, one per channel: `prepare(size, overlap)` in `prepare()` (`1024, 4`); per sample `if (s.push(x))` change bins `0 .. bins()-1`, then `y = s.pull()`, `latency()` samples late (report it). Bins: `magnitude(k)` (a sine of amplitude 1 reads 1), `phase(k)`, `real(k)`, `imag(k)`, `set(k, re, im)`, `setPolar(k, mag, phase)`, `scale(k, gain)`, `binHz(k)` |
| `Random` | `seed(n)`, `next()` (0..1), `bipolar()`, `gaussian()`, `below(n)`; the same sequence everywhere for a seed |
| `Noise` | `seed(n)`; `white()`, `pink()`, `brown()` |

Coefficients are cheap to set in `update()`; setting them per sample (for a
smoothed cutoff, say) works too.

## Compiler messages

Each message has a line, a column, a code and a hint that says what
to write instead:

```
tilt.cpp:12:9: error: integer division needs a positive constant divisor [script.ub.division]
    hint: use idiv(a, b) or imod(a, b)
```

Fix the first error first: later ones often follow from it. The codes group
by area: `script.parse.*` (syntax), `script.unsupported.*` (outside the
subset), `script.type.*`, `script.name.*`, `script.call.*`,
`script.plugin.*` and `script.param.*` (the plugin's shape), `script.ub.*`
(undefined in C++), `script.rt.*` (real-time rules), `script.editor.*`,
`script.shared.*` and `script.format.*` (the editor's rules).
