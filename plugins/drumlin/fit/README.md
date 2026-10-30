# drumlin-fit

Fits Drumlin's recipes to recordings of a drum machine. Drumlin plays no
samples: each sound is a recipe (resonant modes, bands of filtered noise, a
clap's bursts, a hi-hat's loop, and the machine after them), and this tool
finds the recipe that sounds most like a set of recordings.

It renders with Drumlin's own engine, the script compiled here as C++ just as
the stock plugin compiles it, so what it fits is what the plugin plays. It is
built with the tests (`cmake --build build/release --target drumlin-fit`;
build it optimized, as the release preset does) and never run by them: the
recordings are not part of the repository, and nothing taken from them may
be. Only the fitted numbers go into the script.

## The machine it assumes

A recording is what a voice's EPROM held, played by its DAC at a clock: the
LM-1's tuning knob sets that clock, so each pitch number of a sample bank is
the same recording, faster or slower. A recipe's frequencies and times are
given at its `clock` (Tune 0); a recording of the machine at another setting
is matched by playing the recipe at that recording's clock. The DAC is 8-bit
mu-law companded with unsmoothed steps; a low-pass filter that closes after
the hit, the output's coupling and a VCA follow, in real time. See the
script's opening comment and docs/specs/drumlin.md.

## Specs

A spec is a text file, one statement a line, `#` for comments:

| Line | Meaning |
|---|---|
| `set <field> <value>` | a recipe field held fixed (`modes 6`, `band1.shape 2`, `clock 22583.7`) |
| `free <field> <start> <low> <high> [log]` | a field the fit searches, from `start`, within the range (log: a ratio scale) |
| `free peak <start> <low> <high> log` | the recording's peak into the DAC; above 1 it clipped, as the LM-1's kick did |
| `load <file>` | the values of an earlier fit's `.best` (they become the free fields' starts) |
| `target <wav> clock=<Hz> [weight=<w>] [decay=<ms>] [hit=<n>] [accent=1] [upto=<ms>]` | a recording to match: the clock its machine played it at, its weight, a hi-hat's VCA decay, the hit's number (a loop's starting point), its accent, and where its score stops (before a fade the recording was given) |
| `weights <hit> <body> <level> <wave> [<fine> [<crest>]]` | the score's parts (below); `fine` and `crest` 0 if left out |
| `wave <ms>` | how much of the hit the waveform part compares |
| `window <before> <longest>` | seconds compared before the hit, and at most after it |
| `drive auto\|fixed` | whether the drive follows `peak` (auto, the default) or stays as set |

Fields are a recipe's members (`clock`, `length`, `start`, `strike`, `bend`,
`bendMs`, `level`, `vcfOpen`, `vcfShut`, `vcfMs`, `hpf`, `hold`, `decay`,
`accent`, `gate`, `gateMs`, `burstMs`, `burstGain`, `burstJitter`, and the
counts `modes`, `bands`, `bursts`, `loop`), a mode's (`mode3.hz`, `.gain`,
`.fast`, `.slow`, `.split`, `.phase`) and a band's (`band0.hz`, `.q`, `.gain`,
`.delay`, `.attack`, `.fast`, `.slow`, `.split`, and `.shape`: 0 band pass, 1
low pass, 2 high pass).

## The score

Each render is lined up with its recording at the hit (the first sample past a
quarter of the peak) and compared in six parts, each in dB:

- **hit**: band levels (28 bands from 150 Hz) in 6 ms frames around the hit;
- **body**: band levels (44 bands from 25 Hz) in 46 ms frames, later frames
  counting less;
- **level**: the level in 8 ms windows every 2 ms;
- **wave**: one minus the waveform's correlation over the hit's first `wave`
  milliseconds, times ten (for tonal sounds, whose phase can be matched);
- **fine**: the body's spectrum in 21.5 Hz bins, averaged over the whole
  body: what the bands blur, a pure tone standing out of noise or a
  cymbal's narrow peaks (for the hi-hat, whose loop the bands alone let
  turn into a few loud tones);
- **crest**: the peak over the RMS in 5 ms windows through the hit's first
  60 ms: how far its top is clipped flat, which no spectrum shows (the
  bands alone let a recipe be driven far past the DAC's full scale).

Levels below the recording's loudest by 60 or 70 dB count as that floor.
The total is the weighted mean of hit, body, level, fine and crest, plus
wave.

## Running it

```sh
drumlin-fit fit kick.spec --evals 50000 --restarts 3   # writes kick.spec.best
drumlin-fit score view.spec                            # a spec that loads the .best
drumlin-fit render view.spec out/                      # render0.wav ... beside each target
drumlin-fit emit view.spec 0                           # the recipe as the script's code
```

`fit` runs CMA-ES on every core: several searches from the spec's start
(`--restarts`), the best refined by a narrower one. Paste `emit`'s lines into
the script's generated `recipes()` for the sound's index. Read the renders
against the recordings, not only the score: a plot of both waveforms and
spectrograms shows where a recipe is short of something the score misses.
