# Sedge: the note a sound is, and how far off

Use `com.suppleaudio.sedge` to find the pitch of one note at a time: an
instrument being tuned, a recorded string or voice, a sample, a synth patch
you want on a given note. It is chromatic (it names whatever note it hears,
no instrument modes) and reads one note at a time, not chords. It passes the
audio through untouched (unless `mute` is on), adds no latency and reports
what it hears as read-only parameters.

## Reading it

| Where | How |
|---|---|
| DAW, while playing or monitoring | `fx.meters {"fx": "fx_3"}`: the note as it stands now |
| DAW, after a render | `render.mix`, then `fx.meters {"fx": "fx_3", "from": "render"}`: the last note the render held |
| Sandbox | `dsp.test` with `plugin: "com.suppleaudio.sedge"` (a `frequency_hz` sine, or an `audio_file`) reports them as `meters` at the signal's end |

| Key | Unit | Meaning |
|---|---|---|
| `note` | choice | `none` until a note is heard, then `c`, `d_flat`, `d`, `e_flat`, `e`, `f`, `f_sharp`, `g`, `a_flat`, `a`, `b_flat`, `b` |
| `octave` | 0..8 | As in A4, the A above middle C (440 Hz) |
| `cents` | ct | The pitch against that note: positive is sharp, −50..50 |
| `frequency` | Hz | The pitch heard; 0 until one is |

**It holds the last note.** Below `threshold`, or while the sound is not
clearly pitched (noise, a chord, a pluck's first scrape), the readings keep
what they last showed. So a reading after a sound ends is the end of that
sound. Read it while the note sounds, or render a passage that ends on the
note you care about. `note: none` means nothing pitched has been heard yet.

Readings come every 15 ms over the last `window` (40 ms by default).
Smoothing settles in about 50 ms, so a reading reflects roughly the last
tenth of a second.

## Settings

| Key | Range | Default | Use |
|---|---|---|---|
| `reference` | 415..466 Hz | 440 | A4's pitch: 442 for many orchestras, 415 for baroque |
| `temperament` | `equal`, `just`, `pythagorean`, `meantone`, `werckmeister3`, `vallotti` | `equal` | What the notes are tuned to; A4 stays at `reference` in all |
| `key` | `c` … `b` | `c` | The note a temperament other than equal is built on |
| `window` | 10..200 ms | 40 | Longer is steadier and reads low notes surely (80–100 ms for a bass's low strings), shorter answers sooner |
| `tolerance` | 1..10 ct | 3 | Within this the editor shows the note in tune; the readouts are unaffected |
| `threshold` | −90..−20 dBFS | −60 | Raise it above hum or bleed so the display holds between notes |
| `mute` | | false | Silences the output, for tuning on stage |

Range: about A0 (27.5 Hz) to C7 (2093 Hz). Expect a settled reading within
a few tenths of a cent of a steady tone. A plucked string starts a few cents
sharp and settles within about 0.2 s.

## Recipes

Is this sample in tune, and what note is it?

```json
{"command": "dsp.test", "args": {"plugin": "com.suppleaudio.sedge",
  "audio_file": "/path/to/sample.wav"}}
```

Read `meters` for the note at the file's end. For a sample that decays to
noise, that is the last pitched moment, which is what you want.

Tune a synth patch's oscillator to a note: put Sedge after the synth, play
a held note, read `cents` with `fx.meters`, and change the synth's fine tune
by minus that many cents.

Check a string section against A442 in Vallotti on D:

```json
{"params": {"reference": 442, "temperament": "vallotti", "key": "d"}}
```

## Factory patches

Concert A440, A442, Baroque A415, Bass (a 100 ms window), Stage, Muted,
Werckmeister III, Vallotti, Meantone.
