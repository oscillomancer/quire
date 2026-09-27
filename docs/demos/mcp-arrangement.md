# An arrangement over MCP

A scripted MCP client builds four bars with three Orrery tracks, renders, saves, reopens and renders again (bit-identical). Paths point into a temporary work directory.

## tempo_set

Set the tempo.

```json
{
  "points": [
    {
      "beat": 0,
      "bpm": 100
    }
  ]
}
```

Result:

```json
{
  "count": 1
}
```

## batch

Three tracks with Orrery patches, notation MIDI and a basic mix, as one undo step.

```json
{
  "label": "Build a four-bar arrangement",
  "steps": [
    {
      "command": "track.add",
      "args": {
        "name": "Bass"
      },
      "as": "bass"
    },
    {
      "command": "fx.add",
      "args": {
        "track": "${bass.track}",
        "plugin": "com.suppleaudio.orrery",
        "patch": {
          "schema": "supple.orrery-patch/1.0",
          "name": "Round bass",
          "seed": 1,
          "voice": {
            "polyphony": 1,
            "glide": "30 ms"
          },
          "oscillators": {
            "osc1": {
              "type": "va",
              "wave": "saw",
              "to": "filter1"
            },
            "osc3": {
              "type": "sub",
              "wave": "sine",
              "octave": -1,
              "level": "-6 dB",
              "to": "filter1"
            }
          },
          "filters": {
            "filter1": {
              "type": "ladder",
              "cutoff": "400 Hz",
              "resonance": "20 %"
            }
          },
          "envelopes": {
            "amp": {
              "attack": "3 ms",
              "sustain": "90 %",
              "release": "80 ms"
            }
          }
        }
      }
    },
    {
      "command": "item.add",
      "args": {
        "track": "${bass.track}",
        "bar": 1,
        "notation": "voice bass; 1: A1:q A1:q A1:e C2:e E2:q | F1:q F1:q F1:e A1:e C2:q | G1:q G1:q G1:e B1:e D2:q | E1:q E1:q E1:h"
      }
    },
    {
      "command": "track.set",
      "args": {
        "track": "${bass.track}",
        "volume_db": -6,
        "pan": 0
      }
    },
    {
      "command": "track.add",
      "args": {
        "name": "Pad"
      },
      "as": "pad"
    },
    {
      "command": "fx.add",
      "args": {
        "track": "${pad.track}",
        "plugin": "com.suppleaudio.orrery",
        "patch": {
          "schema": "supple.orrery-patch/1.0",
          "name": "Warm pad",
          "seed": 2,
          "voice": {
            "polyphony": 8,
            "unison": 3,
            "unison_detune": "10 ct"
          },
          "oscillators": {
            "osc1": {
              "type": "va",
              "wave": "saw",
              "to": "filter1"
            }
          },
          "filters": {
            "filter1": {
              "type": "svf",
              "cutoff": "1.5 kHz"
            }
          },
          "envelopes": {
            "amp": {
              "attack": "300 ms",
              "sustain": "80 %",
              "release": "600 ms"
            }
          },
          "fx": {
            "chain": [
              "chorus",
              "reverb"
            ]
          }
        }
      }
    },
    {
      "command": "item.add",
      "args": {
        "track": "${pad.track}",
        "bar": 1,
        "notation": "voice pad; 1: [A2 C3 E3]:w | [F2 A2 C3]:w | [G2 B2 D3]:w | [E2 G#2 B2]:w"
      }
    },
    {
      "command": "track.set",
      "args": {
        "track": "${pad.track}",
        "volume_db": -12,
        "pan": -20
      }
    },
    {
      "command": "track.add",
      "args": {
        "name": "Lead"
      },
      "as": "lead"
    },
    {
      "command": "fx.add",
      "args": {
        "track": "${lead.track}",
        "plugin": "com.suppleaudio.orrery",
        "patch": {
          "schema": "supple.orrery-patch/1.0",
          "name": "Bright lead",
          "seed": 3,
          "oscillators": {
            "osc1": {
              "type": "va",
              "wave": "square",
              "to": "filter1"
            }
          },
          "filters": {
            "filter1": {
              "type": "svf",
              "cutoff": "3 kHz"
            }
          },
          "envelopes": {
            "amp": {
              "attack": "5 ms",
              "sustain": "70 %",
              "release": "150 ms"
            }
          },
          "fx": {
            "chain": [
              "delay"
            ]
          }
        }
      }
    },
    {
      "command": "item.add",
      "args": {
        "track": "${lead.track}",
        "bar": 1,
        "notation": "voice lead; 1: E4:q A4:q C5:h | A4:q F4:q C5:h | B4:q G4:q D5:h | B4:w"
      }
    },
    {
      "command": "track.set",
      "args": {
        "track": "${lead.track}",
        "volume_db": -9,
        "pan": 25
      }
    }
  ]
}
```

Result:

```json
{
  "label": "Build a four-bar arrangement",
  "results": [
    {
      "track": "tr_1"
    },
    {
      "fx": "fx_1",
      "name": "Orrery",
      "semantic": true,
      "chain": [
        "fx_1"
      ]
    },
    {
      "item": "it_1",
      "take": "tk_1",
      "source": "mi_1",
      "position": 0.0,
      "length": 16.0
    },
    {},
    {
      "track": "tr_2"
    },
    {
      "fx": "fx_2",
      "name": "Orrery",
      "semantic": true,
      "chain": [
        "fx_2"
      ]
    },
    {
      "item": "it_2",
      "take": "tk_2",
      "source": "mi_2",
      "position": 0.0,
      "length": 16.0
    },
    {},
    {
      "track": "tr_3"
    },
    {
      "fx": "fx_3",
      "name": "Orrery",
      "semantic": true,
      "chain": [
        "fx_3"
      ]
    },
    {
      "item": "it_3",
      "take": "tk_3",
      "source": "mi_3",
      "position": 0.0,
      "length": 16.0
    },
    {}
  ]
}
```

## session_describe

Look at the session.

```json
{}
```

Result:

```json
{
  "text": "Project \"\" \u00b7 48 kHz \u00b7 100 BPM 4/4 \u00b7 9.6 s\ntr_1 Bass \u00b7 vol -6.0 dB \u00b7 fx: fx_1 Orrery\n    it_1 bar 1\u2013bar 5 (MIDI mi_1, 18 notes)\ntr_2 Pad \u00b7 vol -12.0 dB \u00b7 pan -20 \u00b7 fx: fx_2 Orrery\n    it_2 bar 1\u2013bar 5 (MIDI mi_2, 12 notes)\ntr_3 Lead \u00b7 vol -9.0 dB \u00b7 pan 25 \u00b7 fx: fx_3 Orrery\n    it_3 bar 1\u2013bar 5 (MIDI mi_3, 10 notes)\n"
}
```

## render_mix

Render the four bars with a one-second tail.

```json
{
  "name": "arrangement",
  "end_beat": 16,
  "tail_s": 1.0
}
```

Result:

```json
{
  "path": "<workdir>/renders/arrangement.wav",
  "seconds": 10.6,
  "peak_dbfs": -17.35,
  "output_latency": 0
}
```

## project_save

Save the project.

```json
{
  "path": "arrangement.json"
}
```

Result:

```json
{
  "path": "<workdir>/arrangement.json"
}
```

## project_open

Reopen it.

```json
{
  "path": "arrangement.json"
}
```

Result:

```json
{
  "name": "",
  "path": "<workdir>/arrangement.json",
  "branch": "main",
  "sample_rate": 48000.0,
  "tempo": 100.0,
  "meter": "4/4",
  "version": 0,
  "recoverable": 0
}
```

## render_mix

Render the four bars with a one-second tail.

```json
{
  "name": "reopened",
  "end_beat": 16,
  "tail_s": 1.0
}
```

Result:

```json
{
  "path": "<workdir>/renders/reopened.wav",
  "seconds": 10.6,
  "peak_dbfs": -17.35,
  "output_latency": 0
}
```
