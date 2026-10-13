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
- One struct derives from `Effect`: the plugin. Others are plain structs it
  holds as members. Only effects for now (audio in, audio out): scripts
  receive no notes yet, so `Instrument` and `NoteEffect` are refused.
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

| Modifier | Effect |
|---|---|
| `.help("one sentence")` | The tooltip and the schema's description. Write one for every parameter |
| `.in("Group")` | A section in the editor and in hosts |
| `.smoothed(ms)` | The framework glides it per sample; read `p.at(i)` in `process` |
| `.offAtMin()` | Its minimum means off ("-inf dB"); `.gain()` is exactly 0 there |
| `.meter()` | An output the plugin reports with `p.set(v)`: read-only, never saved |
| `.modulated()`, `.notAutomatable()` | As named |
| `.role({"level"})` | What it does, for agents: `level`, `time`, `brightness` ... |
| `.curved(Curve::Log)`, `.curved(Curve::Pow, k)` | Knob travel |
| `.withKey("key")`, `.withId(n)` | Override the patch key or the CLAP id (see below) |

Reading: a `Param` reads as a `double` in its unit (`gain * 0.5`), with
`.value()`, `.at(i)` (the glide at frame `i`), `.index()`, `.on()`,
`.gain()` (dB as linear gain) and, in `update()`, `.changed()`.

**Keys and IDs.** A parameter's patch key is its member's name in
snake_case (`lowCut` is `low_cut`), and its CLAP id is a hash of the key.
Renaming a member changes both, and sessions that saved the old one lose
that value: give the new name `.withKey("old_key")` to keep it.

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

There is no `reset()` to write: the runtime returns every member to what
`prepare()` left, by itself. The script never calls a hook.

`Block`: `io.channels`, `io.frames` (a block may be a single frame),
`io[c][i]` (read the input, write the output, in place), `io.transport`
(`playing`, `tempo`, `beats`, `barStart`, time signature),
`io.sidechain(c)[i]`, `io.output(bus, c)[i]`, `io.sidechainConnected`.
From any member function: `sampleRate()`, `maxBlockSize()`, `channels()`,
`offline()`.

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
spends it is stopped and silent until it is reloaded, and the log names the
line.

## Real time

`process()`, and everything it calls, never allocates: `DelayLine::prepare`
and `Buffer::resize` belong in `prepare()`, and the compiler says so
anywhere else. There is no file, network or clock access at all.

## The runtime library

Math (all in double; `min`, `max`, `clamp`, `abs` also take ints): `sin`
`cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `exp` `exp2`
`log` `log2` `log10` `pow` `sqrt` `cbrt` `floor` `ceil` `trunc` `round`
`fmod` `hypot` `abs` `min` `max` `clamp` `lerp(a, b, t)` `sign` `isFinite`,
and the constants `pi`, `twoPi`.

Conversions: `dbToGain(db)`, `gainToDb(g)` (floored at -120), `mtof(note)`,
`ftom(hz)`, `toInt` `floorToInt` `ceilToInt` `roundToInt`, `idiv` `imod`
`wrap`.

DSP objects (each starts silent; `clear()` returns it there):

| Object | Use |
|---|---|
| `Biquad` | `lowpass` `highpass` `bandpass` `notch` `allpass` `(hz, q)`, `peak` `lowShelf` `highShelf` `(hz, q, gainDb)`; `process(x)`; `responseDb(hz)` |
| `OnePole` | `setCutoff(hz)`; `lowpass(x)`, `highpass(x)` |
| `DelayLine` | `prepare(maxSamples)` in `prepare()`; `push(x)` once per frame; `read(delaySamples)` (fractional, 0 is the sample just pushed) |
| `Oscillator` | `setShape(Oscillator::Shape::Saw)` (Sine, Saw, Square, Triangle, anti-aliased), `setFrequency(hz)`, `setPulseWidth`, `setPhase`, `next()` |
| `Adsr` | `set(attackMs, decayMs, sustain, releaseMs)`, `gate(on)`, `next()`, `active()` |
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
(undefined in C++), `script.rt.*` (real-time rules).
