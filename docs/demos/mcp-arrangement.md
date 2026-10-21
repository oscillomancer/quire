# An arrangement over MCP

A scripted MCP client builds four bars with three synth tracks, renders, saves, reopens and renders again (bit-identical). Paths point into a temporary work directory.

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

Three tracks with synth patches, notation MIDI and a basic mix, as one undo step.

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
        "plugin": "com.suppleaudio.marram",
        "patch": {
          "schema": "supple.marram-patch/1.0",
          "name": "Round bass",
          "params": {
            "voice.mode": "mono",
            "voice.glide": "30 ms",
            "osc2.type": "sub",
            "osc2.wave": "sine",
            "osc2.level": "-6 dB",
            "filter1.cutoff": "400 Hz",
            "filter1.resonance": "20 %",
            "amp.attack": "3 ms",
            "amp.sustain": "90 %",
            "amp.release": "80 ms"
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
        "plugin": "com.suppleaudio.marram",
        "patch": {
          "schema": "supple.marram-patch/1.0",
          "name": "Warm pad",
          "params": {
            "voice.unison": 3,
            "voice.unison_detune": "10 ct",
            "filter1.type": "svf",
            "filter1.cutoff": "1.5 kHz",
            "amp.attack": "300 ms",
            "amp.sustain": "80 %",
            "amp.release": "600 ms"
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
        "plugin": "com.suppleaudio.marram",
        "patch": {
          "schema": "supple.marram-patch/1.0",
          "name": "Bright lead",
          "params": {
            "osc1.wave": "square",
            "filter1.type": "svf",
            "filter1.cutoff": "3 kHz",
            "amp.attack": "5 ms",
            "amp.sustain": "70 %",
            "amp.release": "150 ms"
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
      "name": "Marram",
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
      "name": "Marram",
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
      "name": "Marram",
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
  "text": "Project \"\" \u00b7 48 kHz \u00b7 100 BPM 4/4 \u00b7 9.6 s\ntr_0 Master [master]\n  tr_1 Bass [channel] \u00b7 vol -6.0 dB \u00b7 fx: fx_1 Marram\n    it_1 bar 1\u2013bar 5 (MIDI mi_1, 18 notes)\n  tr_2 Pad [channel] \u00b7 vol -12.0 dB \u00b7 pan -20 \u00b7 fx: fx_2 Marram\n    it_2 bar 1\u2013bar 5 (MIDI mi_2, 12 notes)\n  tr_3 Lead [channel] \u00b7 vol -9.0 dB \u00b7 pan 25 \u00b7 fx: fx_3 Marram\n    it_3 bar 1\u2013bar 5 (MIDI mi_3, 10 notes)\n"
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
  "render": "rd_1",
  "path": "<workdir>/renders/arrangement.wav",
  "seconds": 10.6,
  "peak_dbfs": -19.02,
  "output_latency": 3
}
```

## analyze_render

Measure the render.

```json
{
  "render": "rd_1"
}
```

Result:

```json
{
  "schema": "supple.analysis-report/1.0",
  "source": {
    "seconds": 10.6,
    "sample_rate": 48000.0,
    "channels": 2,
    "layout": "stereo"
  },
  "loudness": {
    "integrated_lufs": -29.9,
    "lra_lu": 1.3,
    "max_momentary_lufs": -28.5,
    "max_short_term_lufs": -29.2,
    "true_peak_dbtp": -19.0,
    "sample_peak_dbfs": -19.0,
    "plr_lu": 10.8
  },
  "spectral": {
    "centroid_hz": 948.0,
    "rolloff_hz": 492.0,
    "flatness": 0.0,
    "flux": 0.22,
    "tilt_db_per_oct": -3.4,
    "bands": {
      "sub": -5.0,
      "low": -6.0,
      "low_mid": -6.7,
      "mid": -7.0,
      "high_mid": -18.6,
      "presence": -26.3,
      "air": -31.1
    }
  },
  "harmonic": {
    "pitched": false,
    "voiced_pct": 11.7
  },
  "envelope": {
    "attack_ms": 307.0,
    "decay20_ms": 4890.0,
    "crest_db": 11.7,
    "onsets_per_s": 0.1
  },
  "stereo": {
    "correlation": 0.916,
    "width": 0.043,
    "balance_db": 0.6,
    "mono_loss_lu": 0.2
  },
  "summary": "-29.9 LUFS (LRA 1.3 LU), true peak -19.0 dBTP. Unpitched. Centroid 948 Hz, tilt -3.4 dB/oct, most energy in sub. Attack 307 ms, -20 dB after 4892 ms. Width 0.04, correlation 0.92."
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
  "generation": 2,
  "recoverable": 0,
  "autosave_available": false
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
  "render": "rd_2",
  "path": "<workdir>/renders/reopened.wav",
  "seconds": 10.6,
  "peak_dbfs": -19.02,
  "output_latency": 3
}
```
