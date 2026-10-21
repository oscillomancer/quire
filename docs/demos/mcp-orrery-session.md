# Orrery over MCP

This is a scripted sound-design session with the real Orrery CLAP module. The
same commands run directly in the sandbox and through its Unix socket proxy.
The client checks every step's structured result before recording the exchange.
Arrays longer than 12 entries show their count and names; temporary paths,
render IDs, and elapsed job times use stable placeholders. MCP text content
that exactly duplicates structuredContent is shown once.

## stdio

### request

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-06-18",
    "capabilities": {},
    "clientInfo": {
      "name": "orrery-demo",
      "version": "1.0"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-06-18",
    "capabilities": {
      "tools": {}
    },
    "serverInfo": {
      "name": "quire",
      "version": "0.1.0"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/list",
  "params": {}
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": {
      "_elided_array_count": 31,
      "names": [
        "analyze",
        "batch",
        "compare",
        "dsp_check",
        "dsp_export",
        "dsp_get",
        "dsp_list",
        "dsp_preview",
        "dsp_test",
        "dsp_write",
        "masking",
        "optimize_start",
        "param_get",
        "param_set",
        "patch_get",
        "patch_merge",
        "patch_set",
        "plugin_describe",
        "plugin_list",
        "plugin_load",
        "plugin_remove",
        "preset_list",
        "preset_load",
        "preset_save",
        "profile_evaluate",
        "render",
        "session_get",
        "session_reset",
        "session_set",
        "describe",
        "exec"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "describe",
    "arguments": {
      "command": "patch.merge"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "name": "patch.merge",
      "version": 1,
      "kind": "edit",
      "exposure": "mcp-tool",
      "summary": "Merge a partial patch (RFC 7396) into an instance.",
      "description": "patch: only the values to change, e.g. {\"params\": {\"filter.cutoff\": \"800 Hz\"}}; null removes a value (back to its default). Returns the effective change (a minimal merge patch; {} if nothing changed) and a description of the new sound. inactive names the params it set that do not apply with the new values: they are kept, and heard once what their active_when names allows it (plugin.describe detail full).",
      "tags": [
        "patch"
      ],
      "args": {
        "type": "object",
        "title": "patch_merge_args",
        "properties": {
          "instance": {
            "type": "string",
            "pattern": "^in_[0-9]+$",
            "description": "Instance id from plugin.load, e.g. \"in_1\"."
          },
          "patch": {}
        },
        "required": [
          "instance",
          "patch"
        ],
        "additionalProperties": false
      },
      "result": {
        "type": "object",
        "title": "patch_merge",
        "properties": {
          "changed": {},
          "description": {
            "type": "string"
          },
          "inactive": {
            "type": "array",
            "items": {
              "type": "string"
            },
            "description": "Params set that do not apply now"
          }
        },
        "required": [
          "changed",
          "description"
        ],
        "additionalProperties": false
      },
      "errors": [
        {
          "code": "sandbox.not_semantic",
          "when": "the plugin has no semantic patch",
          "hint": "use param.set"
        },
        {
          "code": "params.out_of_range",
          "when": "a value is outside its range",
          "hint": "the hint gives the valid range"
        }
      ],
      "examples": [
        {
          "title": "Turn it down",
          "args": {
            "instance": "in_2",
            "patch": {
              "params": {
                "gain": "-12 dB"
              }
            }
          },
          "result": {
            "changed": {
              "params": {
                "gain": -12.0
              }
            },
            "description": "<any>"
          },
          "fixture": "synth_loaded"
        }
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "plugin_load",
    "arguments": {
      "plugin": "<orrery.clap>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "instance": "in_1",
      "name": "Orrery",
      "semantic": true,
      "chain": [
        "in_1"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "tools/call",
  "params": {
    "name": "plugin_describe",
    "arguments": {
      "instance": "in_1",
      "detail": "summary"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "plugin": "com.suppleaudio.orrery",
      "name": "Orrery",
      "params": {
        "Voice": [
          "voice.mode = Poly  (poly|mono|legato)",
          "voice.polyphony = 8  (1..32)",
          "voice.unison = 1  (1..8)",
          "voice.unison_detune = 10.0 ct  (0.0..100.0 ct)",
          "voice.unison_spread = 50.0 %  (0.0..100.0 %)",
          "voice.glide = 0.0 ms  (0.0..5000.0 ms, pow:3)",
          "voice.glide_mode = Legato  (always|legato)",
          "voice.bend_range = 2 st  (0..48 st)",
          "voice.mpe = off  (on|off)",
          "voice.drift = 0.0 %  (0.0..100.0 %)"
        ],
        "Oscillator 1": [
          "osc1.level = -6.00 dB  (-60.0..6.0 dB)",
          "osc1.pan = 0.0 %  (-100.0..100.0 %)",
          "osc1.coarse = 0 st  (-48..48 st)",
          "osc1.fine = 0.0 ct  (-100.0..100.0 ct)",
          "osc1.phase_mode = Free  (free|reset)",
          "osc1.phase = 0.0 deg  (0.0..360.0 deg)",
          "osc1.wave = Saw  (saw|square|pulse|triangle|sine|half_sine|abs_sine|square_sine)",
          "osc1.width = 50.0 %  (1.0..99.0 %)",
          "osc1.shape = 0.0 %  (0.0..100.0 %)"
        ],
        "Amp": [
          "amp.level = 0.0 dB  (-60.0..12.0 dB)",
          "amp.velocity = 50.0 %  (0.0..100.0 %)",
          "amp.pan = 0.0 %  (-100.0..100.0 %)"
        ],
        "Amp envelope": [
          "amp.delay = 0.0 ms  (0.0..10000.0 ms, pow:3)",
          "amp.attack = 5.00 ms  (0.0..30000.0 ms, pow:3)",
          "amp.hold = 0.0 ms  (0.0..10000.0 ms, pow:3)",
          "amp.decay = 300 ms  (1.0..30000.0 ms, pow:3)",
          "amp.sustain = 70.0 %  (0.0..100.0 %)",
          "amp.release = 300 ms  (1.0..30000.0 ms, pow:3)",
          "amp.attack_curve = 0.0 %  (-100.0..100.0 %)",
          "amp.decay_curve = +50.0 %  (-100.0..100.0 %)",
          "amp.release_curve = +50.0 %  (-100.0..100.0 %)",
          "amp.retrigger = Always  (always|legato)"
        ],
        "Output": [
          "output.level = 0.0 dB  (-60.0..12.0 dB)"
        ]
      },
      "patch_type": "supple.orrery-patch/1.0",
      "description": "Poly. osc1 va saw. Amp A 5.00 ms D 300 ms S 70.0 % R 300 ms.",
      "docs": "Orrery is a constrained-modular synthesizer. A patch (supple.orrery-patch/1.0) chooses which slots exist — oscillators osc1..osc4 (va, wavetable, fm, noise, sub), filters filter1/filter2 (ladder, svf, comb, formant; serial or parallel), drive, envelopes amp/env2..env4, lfos lfo1..lfo4, steps step1/step2, eight macros and up to five post-FX in fx.chain — and routes modulation as \"<source>><destination>\": {\"amount\": ...} in the destination's unit (oct for frequencies, st for pitch, dB for levels, x for oscN.fm, % otherwise). Values are CLAP parameters keyed by patch path (osc1.level, filter1.cutoff, amp.attack); structure lives only in the patch. Macros are the automation surface for modulation.",
      "status_available": false,
      "inactive": {
        "_elided_array_count": 187
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "method": "tools/call",
  "params": {
    "name": "patch_set",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "schema": "supple.orrery-patch/1.0",
        "name": "MCP demo pad",
        "seed": 1234,
        "oscillators": {
          "osc1": {
            "type": "va",
            "wave": "saw"
          }
        },
        "filters": {
          "filter1": {
            "type": "svf",
            "cutoff": "900 Hz"
          }
        },
        "envelopes": {
          "amp": {
            "attack": "80 ms",
            "release": "200 ms"
          },
          "env2": {
            "attack": "100 ms",
            "decay": "400 ms",
            "sustain": "0 %"
          }
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "description": "Poly. osc1 va saw → svf lowpass 900 Hz. env2 (A 100 ms D 400 ms S 0.0 % R 300 ms). Amp A 80.0 ms D 300 ms S 70.0 % R 200 ms."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "base pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-1>",
      "path": "<workdir>/renders/<render-1>.wav",
      "seconds": 2.3000000000000003,
      "peak_dbfs": -17.09,
      "tail_s": 0.3,
      "tail_capped": true,
      "latency_samples": 0,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -23.8,
          "lra_lu": null,
          "max_momentary_lufs": -23.0,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -17.1,
          "sample_peak_dbfs": -17.1,
          "plr_lu": 6.7
        },
        "spectral": {
          "centroid_hz": 560.0,
          "rolloff_hz": 328.0,
          "flatness": 0.0,
          "flux": 0.005,
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": -46.6,
            "low": -1.2,
            "low_mid": -7.5,
            "mid": -12.2,
            "high_mid": -34.8,
            "presence": -49.2,
            "air": -57.4
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3-1c",
          "stability_cents": 460.7,
          "harmonics_db": [
            0.0,
            -6.6,
            -11.1,
            -15.2,
            -18.5,
            -22.0,
            -24.8,
            -27.5
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.6
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.7,
          "crest_db": 8.9,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-23.8 LUFS, true peak -17.1 dBTP. Pitched E3-1c (165 Hz), 2nd/3rd harmonics -6.6/-11.1 dB. Centroid 560 Hz, tilt -9.5 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1020 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-1>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -23.8,
          "lra_lu": null,
          "max_momentary_lufs": -23.0,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -17.1,
          "sample_peak_dbfs": -17.1,
          "plr_lu": 6.7
        },
        "spectral": {
          "centroid_hz": {
            "mean": 553.0,
            "median": 560.0,
            "p10": 515.0,
            "p90": 599.0
          },
          "rolloff_hz": {
            "mean": 328.0,
            "median": 328.0,
            "p10": 281.0,
            "p90": 398.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.0
          },
          "flux": {
            "mean": 0.025,
            "median": 0.005,
            "p10": 0.0,
            "p90": 0.058
          },
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": {
              "level_db": -46.6,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.2,
              "share_pct": 76.0,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.5,
              "share_pct": 18.0,
              "width": 0.0
            },
            "mid": {
              "level_db": -12.2,
              "share_pct": 6.0,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -34.8,
              "share_pct": 0.0,
              "width": 0.0
            },
            "presence": {
              "level_db": -49.2,
              "share_pct": 0.0,
              "width": 0.0
            },
            "air": {
              "level_db": -57.4,
              "share_pct": 0.0,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3-1c",
          "stability_cents": 460.7,
          "harmonics_db": [
            0.0,
            -6.6,
            -11.1,
            -15.2,
            -18.5,
            -22.0,
            -24.8,
            -27.5
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.6
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.7,
          "crest_db": 8.9,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-23.8 LUFS, true peak -17.1 dBTP. Pitched E3-1c (165 Hz), 2nd/3rd harmonics -6.6/-11.1 dB. Centroid 560 Hz, tilt -9.5 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1020 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "filters": {
          "filter1": {
            "cutoff": "1.4 kHz"
          }
        },
        "routes": {
          "env2>filter1.cutoff": {
            "amount": "+2 oct"
          }
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "filters": {
          "filter1": {
            "cutoff": 1400.0
          }
        },
        "routes": {
          "env2>filter1.cutoff": {
            "amount": 2.0
          }
        }
      },
      "description": "Poly. osc1 va saw → svf lowpass 1.40 kHz. env2 (A 100 ms D 400 ms S 0.0 % R 300 ms). env2 → filter1.cutoff +2.00 oct. Amp A 80.0 ms D 300 ms S 70.0 % R 200 ms."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "opened pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-2>",
      "path": "<workdir>/renders/<render-2>.wav",
      "seconds": 2.3000000000000003,
      "peak_dbfs": -16.330000000000002,
      "tail_s": 0.3,
      "tail_capped": true,
      "latency_samples": 0,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -23.1,
          "lra_lu": null,
          "max_momentary_lufs": -22.1,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -16.3,
          "sample_peak_dbfs": -16.3,
          "plr_lu": 6.8
        },
        "spectral": {
          "centroid_hz": 791.0,
          "rolloff_hz": 398.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -5.8,
          "bands": {
            "sub": -47.1,
            "low": -1.6,
            "low_mid": -7.2,
            "mid": -9.4,
            "high_mid": -23.0,
            "presence": -32.7,
            "air": -38.5
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3-1c",
          "stability_cents": 460.6,
          "harmonics_db": [
            0.0,
            -6.2,
            -10.1,
            -13.1,
            -15.6,
            -17.8,
            -19.9,
            -21.8
          ],
          "inharmonicity": 0.0,
          "hnr_db": 47.0
        },
        "envelope": {
          "attack_ms": 63.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.9,
          "crest_db": 9.0,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-23.1 LUFS, true peak -16.3 dBTP. Pitched E3-1c (165 Hz), 2nd/3rd harmonics -6.2/-10.1 dB. Centroid 791 Hz, tilt -5.8 dB/oct, most energy in low. Attack 63 ms, -20 dB after 1020 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "method": "tools/call",
  "params": {
    "name": "compare",
    "arguments": {
      "reference": "<render-1>",
      "test": "<render-2>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "notable": {
        "_elided_array_count": 18
      },
      "ltas_rms_db": 7.8,
      "summary": "spectral.bands.air.level_db +19.0 dB; spectral.centroid_hz.p90 +739.4 Hz; spectral.bands.presence.level_db +16.5 dB; spectral.bands.high_mid.level_db +11.8 dB; spectral.centroid_hz.mean +352.6 Hz. Timbre difference 7.8 dB (LTAS RMS, level removed).",
      "loss": {
        "total": 80.36952957891427,
        "terms": {
          "bands": 13.927822043692457,
          "centroid": 1.9728050065375413,
          "envelope.attack": 0.04940556564455911,
          "envelope.decay20": 0.0,
          "envelope.release": 0.0,
          "flatness": 1.7709094722703791e-09,
          "flux": 0.0003809826034800333,
          "harmonics": 0.4600659303701549,
          "inharmonicity": 4.935881297447454e-09,
          "ltas": 6.748821628183759,
          "stereo": 0.0,
          "trajectory": 57.210228415175536
        },
        "note_off_s": {
          "reference": 1.9999999999999998,
          "test": 1.9999999999999998
        }
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "tools/call",
  "params": {
    "name": "optimize_start",
    "arguments": {
      "instance": "in_1",
      "params": [
        "filter1.cutoff"
      ],
      "target": {
        "profile": {
          "schema": "supple.analysis-profile/1.0",
          "targets": {
            "spectral.centroid_hz.median": {
              "target": 1500,
              "tolerance": 100
            }
          }
        }
      },
      "phrase": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "evaluations": 20,
      "max_seconds": 120,
      "seed": 2
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "running",
      "params": [
        "filter1.cutoff"
      ],
      "render": "<render-3>"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "method": "tools/call",
  "params": {
    "name": "exec",
    "arguments": {
      "command": "job.wait",
      "args": {
        "job": "jb_1",
        "timeout_s": 30
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "done",
      "progress": {
        "best_loss": 0.2505,
        "evaluations": 17,
        "fraction": 1.0,
        "message": "generation 4"
      },
      "result": {
        "loss": {
          "initial": 50.3315,
          "best": 0.2505,
          "terms": {
            "profile.spectral.centroid_hz.median": 0.2505
          }
        },
        "evaluations": 17,
        "failed": 0,
        "seconds": "<elapsed-job-seconds>",
        "stopped": "evaluations",
        "patch": {
          "params": {
            "filter1.cutoff": 3723.071911
          }
        },
        "changes": [
          {
            "key": "filter1.cutoff",
            "from": 1400.0,
            "to": 3723.0719,
            "unit": "Hz"
          }
        ],
        "render": "<render-3>",
        "applied": false
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "params": {
          "filter1.cutoff": 3723.071911
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "filters": {
          "filter1": {
            "cutoff": 3723.071911
          }
        }
      },
      "description": "Poly. osc1 va saw → svf lowpass 3.72 kHz. env2 (A 100 ms D 400 ms S 0.0 % R 300 ms). env2 → filter1.cutoff +2.00 oct. Amp A 80.0 ms D 300 ms S 70.0 % R 200 ms."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "optimized pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-4>",
      "path": "<workdir>/renders/<render-4>.wav",
      "seconds": 2.3000000000000003,
      "peak_dbfs": -15.83,
      "tail_s": 0.3,
      "tail_capped": true,
      "latency_samples": 0,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.6,
          "lra_lu": null,
          "max_momentary_lufs": -21.6,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.8,
          "sample_peak_dbfs": -15.8,
          "plr_lu": 6.8
        },
        "spectral": {
          "centroid_hz": 1450.0,
          "rolloff_hz": 492.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -4.2,
          "bands": {
            "sub": -47.4,
            "low": -1.9,
            "low_mid": -7.3,
            "mid": -8.4,
            "high_mid": -18.3,
            "presence": -25.4,
            "air": -28.4
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3",
          "stability_cents": 460.9,
          "harmonics_db": [
            0.0,
            -6.1,
            -9.6,
            -12.2,
            -14.2,
            -15.9,
            -17.4,
            -18.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.5,
          "crest_db": 9.2,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-22.6 LUFS, true peak -15.8 dBTP. Pitched E3 (165 Hz), 2nd/3rd harmonics -6.1/-9.6 dB. Centroid 1450 Hz, tilt -4.2 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1024 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-4>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.6,
          "lra_lu": null,
          "max_momentary_lufs": -21.6,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.8,
          "sample_peak_dbfs": -15.8,
          "plr_lu": 6.8
        },
        "spectral": {
          "centroid_hz": {
            "mean": 1650.0,
            "median": 1450.0,
            "p10": 1280.0,
            "p90": 2370.0
          },
          "rolloff_hz": {
            "mean": 501.0,
            "median": 492.0,
            "p10": 398.0,
            "p90": 609.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.001
          },
          "flux": {
            "mean": 0.025,
            "median": 0.006,
            "p10": 0.001,
            "p90": 0.061
          },
          "tilt_db_per_oct": -4.2,
          "bands": {
            "sub": {
              "level_db": -47.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.9,
              "share_pct": 64.9,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.3,
              "share_pct": 18.8,
              "width": 0.0
            },
            "mid": {
              "level_db": -8.4,
              "share_pct": 14.3,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -18.3,
              "share_pct": 1.5,
              "width": 0.0
            },
            "presence": {
              "level_db": -25.4,
              "share_pct": 0.3,
              "width": 0.0
            },
            "air": {
              "level_db": -28.4,
              "share_pct": 0.1,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3",
          "stability_cents": 460.9,
          "harmonics_db": [
            0.0,
            -6.1,
            -9.6,
            -12.2,
            -14.2,
            -15.9,
            -17.4,
            -18.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.5,
          "crest_db": 9.2,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-22.6 LUFS, true peak -15.8 dBTP. Pitched E3 (165 Hz), 2nd/3rd harmonics -6.1/-9.6 dB. Centroid 1450 Hz, tilt -4.2 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1024 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

## proxy

### request

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-06-18",
    "capabilities": {},
    "clientInfo": {
      "name": "orrery-demo",
      "version": "1.0"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-06-18",
    "capabilities": {
      "tools": {}
    },
    "serverInfo": {
      "name": "quire",
      "version": "0.1.0"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/list",
  "params": {}
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": {
      "_elided_array_count": 31,
      "names": [
        "analyze",
        "batch",
        "compare",
        "dsp_check",
        "dsp_export",
        "dsp_get",
        "dsp_list",
        "dsp_preview",
        "dsp_test",
        "dsp_write",
        "masking",
        "optimize_start",
        "param_get",
        "param_set",
        "patch_get",
        "patch_merge",
        "patch_set",
        "plugin_describe",
        "plugin_list",
        "plugin_load",
        "plugin_remove",
        "preset_list",
        "preset_load",
        "preset_save",
        "profile_evaluate",
        "render",
        "session_get",
        "session_reset",
        "session_set",
        "describe",
        "exec"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "describe",
    "arguments": {
      "command": "patch.merge"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "name": "patch.merge",
      "version": 1,
      "kind": "edit",
      "exposure": "mcp-tool",
      "summary": "Merge a partial patch (RFC 7396) into an instance.",
      "description": "patch: only the values to change, e.g. {\"params\": {\"filter.cutoff\": \"800 Hz\"}}; null removes a value (back to its default). Returns the effective change (a minimal merge patch; {} if nothing changed) and a description of the new sound. inactive names the params it set that do not apply with the new values: they are kept, and heard once what their active_when names allows it (plugin.describe detail full).",
      "tags": [
        "patch"
      ],
      "args": {
        "type": "object",
        "title": "patch_merge_args",
        "properties": {
          "instance": {
            "type": "string",
            "pattern": "^in_[0-9]+$",
            "description": "Instance id from plugin.load, e.g. \"in_1\"."
          },
          "patch": {}
        },
        "required": [
          "instance",
          "patch"
        ],
        "additionalProperties": false
      },
      "result": {
        "type": "object",
        "title": "patch_merge",
        "properties": {
          "changed": {},
          "description": {
            "type": "string"
          },
          "inactive": {
            "type": "array",
            "items": {
              "type": "string"
            },
            "description": "Params set that do not apply now"
          }
        },
        "required": [
          "changed",
          "description"
        ],
        "additionalProperties": false
      },
      "errors": [
        {
          "code": "sandbox.not_semantic",
          "when": "the plugin has no semantic patch",
          "hint": "use param.set"
        },
        {
          "code": "params.out_of_range",
          "when": "a value is outside its range",
          "hint": "the hint gives the valid range"
        }
      ],
      "examples": [
        {
          "title": "Turn it down",
          "args": {
            "instance": "in_2",
            "patch": {
              "params": {
                "gain": "-12 dB"
              }
            }
          },
          "result": {
            "changed": {
              "params": {
                "gain": -12.0
              }
            },
            "description": "<any>"
          },
          "fixture": "synth_loaded"
        }
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "plugin_load",
    "arguments": {
      "plugin": "<orrery.clap>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "instance": "in_1",
      "name": "Orrery",
      "semantic": true,
      "chain": [
        "in_1"
      ]
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "tools/call",
  "params": {
    "name": "plugin_describe",
    "arguments": {
      "instance": "in_1",
      "detail": "summary"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "plugin": "com.suppleaudio.orrery",
      "name": "Orrery",
      "params": {
        "Voice": [
          "voice.mode = Poly  (poly|mono|legato)",
          "voice.polyphony = 8  (1..32)",
          "voice.unison = 1  (1..8)",
          "voice.unison_detune = 10.0 ct  (0.0..100.0 ct)",
          "voice.unison_spread = 50.0 %  (0.0..100.0 %)",
          "voice.glide = 0.0 ms  (0.0..5000.0 ms, pow:3)",
          "voice.glide_mode = Legato  (always|legato)",
          "voice.bend_range = 2 st  (0..48 st)",
          "voice.mpe = off  (on|off)",
          "voice.drift = 0.0 %  (0.0..100.0 %)"
        ],
        "Oscillator 1": [
          "osc1.level = -6.00 dB  (-60.0..6.0 dB)",
          "osc1.pan = 0.0 %  (-100.0..100.0 %)",
          "osc1.coarse = 0 st  (-48..48 st)",
          "osc1.fine = 0.0 ct  (-100.0..100.0 ct)",
          "osc1.phase_mode = Free  (free|reset)",
          "osc1.phase = 0.0 deg  (0.0..360.0 deg)",
          "osc1.wave = Saw  (saw|square|pulse|triangle|sine|half_sine|abs_sine|square_sine)",
          "osc1.width = 50.0 %  (1.0..99.0 %)",
          "osc1.shape = 0.0 %  (0.0..100.0 %)"
        ],
        "Amp": [
          "amp.level = 0.0 dB  (-60.0..12.0 dB)",
          "amp.velocity = 50.0 %  (0.0..100.0 %)",
          "amp.pan = 0.0 %  (-100.0..100.0 %)"
        ],
        "Amp envelope": [
          "amp.delay = 0.0 ms  (0.0..10000.0 ms, pow:3)",
          "amp.attack = 5.00 ms  (0.0..30000.0 ms, pow:3)",
          "amp.hold = 0.0 ms  (0.0..10000.0 ms, pow:3)",
          "amp.decay = 300 ms  (1.0..30000.0 ms, pow:3)",
          "amp.sustain = 70.0 %  (0.0..100.0 %)",
          "amp.release = 300 ms  (1.0..30000.0 ms, pow:3)",
          "amp.attack_curve = 0.0 %  (-100.0..100.0 %)",
          "amp.decay_curve = +50.0 %  (-100.0..100.0 %)",
          "amp.release_curve = +50.0 %  (-100.0..100.0 %)",
          "amp.retrigger = Always  (always|legato)"
        ],
        "Output": [
          "output.level = 0.0 dB  (-60.0..12.0 dB)"
        ]
      },
      "patch_type": "supple.orrery-patch/1.0",
      "description": "Poly. osc1 va saw. Amp A 5.00 ms D 300 ms S 70.0 % R 300 ms.",
      "docs": "Orrery is a constrained-modular synthesizer. A patch (supple.orrery-patch/1.0) chooses which slots exist — oscillators osc1..osc4 (va, wavetable, fm, noise, sub), filters filter1/filter2 (ladder, svf, comb, formant; serial or parallel), drive, envelopes amp/env2..env4, lfos lfo1..lfo4, steps step1/step2, eight macros and up to five post-FX in fx.chain — and routes modulation as \"<source>><destination>\": {\"amount\": ...} in the destination's unit (oct for frequencies, st for pitch, dB for levels, x for oscN.fm, % otherwise). Values are CLAP parameters keyed by patch path (osc1.level, filter1.cutoff, amp.attack); structure lives only in the patch. Macros are the automation surface for modulation.",
      "status_available": false,
      "inactive": {
        "_elided_array_count": 187
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "method": "tools/call",
  "params": {
    "name": "patch_set",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "schema": "supple.orrery-patch/1.0",
        "name": "MCP demo pad",
        "seed": 1234,
        "oscillators": {
          "osc1": {
            "type": "va",
            "wave": "saw"
          }
        },
        "filters": {
          "filter1": {
            "type": "svf",
            "cutoff": "900 Hz"
          }
        },
        "envelopes": {
          "amp": {
            "attack": "80 ms",
            "release": "200 ms"
          },
          "env2": {
            "attack": "100 ms",
            "decay": "400 ms",
            "sustain": "0 %"
          }
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "description": "Poly. osc1 va saw → svf lowpass 900 Hz. env2 (A 100 ms D 400 ms S 0.0 % R 300 ms). Amp A 80.0 ms D 300 ms S 70.0 % R 200 ms."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "base pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-1>",
      "path": "<workdir>/renders/<render-1>.wav",
      "seconds": 2.3000000000000003,
      "peak_dbfs": -17.09,
      "tail_s": 0.3,
      "tail_capped": true,
      "latency_samples": 0,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -23.8,
          "lra_lu": null,
          "max_momentary_lufs": -23.0,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -17.1,
          "sample_peak_dbfs": -17.1,
          "plr_lu": 6.7
        },
        "spectral": {
          "centroid_hz": 560.0,
          "rolloff_hz": 328.0,
          "flatness": 0.0,
          "flux": 0.005,
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": -46.6,
            "low": -1.2,
            "low_mid": -7.5,
            "mid": -12.2,
            "high_mid": -34.8,
            "presence": -49.2,
            "air": -57.4
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3-1c",
          "stability_cents": 460.7,
          "harmonics_db": [
            0.0,
            -6.6,
            -11.1,
            -15.2,
            -18.5,
            -22.0,
            -24.8,
            -27.5
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.6
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.7,
          "crest_db": 8.9,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-23.8 LUFS, true peak -17.1 dBTP. Pitched E3-1c (165 Hz), 2nd/3rd harmonics -6.6/-11.1 dB. Centroid 560 Hz, tilt -9.5 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1020 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-1>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -23.8,
          "lra_lu": null,
          "max_momentary_lufs": -23.0,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -17.1,
          "sample_peak_dbfs": -17.1,
          "plr_lu": 6.7
        },
        "spectral": {
          "centroid_hz": {
            "mean": 553.0,
            "median": 560.0,
            "p10": 515.0,
            "p90": 599.0
          },
          "rolloff_hz": {
            "mean": 328.0,
            "median": 328.0,
            "p10": 281.0,
            "p90": 398.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.0
          },
          "flux": {
            "mean": 0.025,
            "median": 0.005,
            "p10": 0.0,
            "p90": 0.058
          },
          "tilt_db_per_oct": -9.5,
          "bands": {
            "sub": {
              "level_db": -46.6,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.2,
              "share_pct": 76.0,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.5,
              "share_pct": 18.0,
              "width": 0.0
            },
            "mid": {
              "level_db": -12.2,
              "share_pct": 6.0,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -34.8,
              "share_pct": 0.0,
              "width": 0.0
            },
            "presence": {
              "level_db": -49.2,
              "share_pct": 0.0,
              "width": 0.0
            },
            "air": {
              "level_db": -57.4,
              "share_pct": 0.0,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3-1c",
          "stability_cents": 460.7,
          "harmonics_db": [
            0.0,
            -6.6,
            -11.1,
            -15.2,
            -18.5,
            -22.0,
            -24.8,
            -27.5
          ],
          "inharmonicity": 0.0,
          "hnr_db": 50.6
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.7,
          "crest_db": 8.9,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-23.8 LUFS, true peak -17.1 dBTP. Pitched E3-1c (165 Hz), 2nd/3rd harmonics -6.6/-11.1 dB. Centroid 560 Hz, tilt -9.5 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1020 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "filters": {
          "filter1": {
            "cutoff": "1.4 kHz"
          }
        },
        "routes": {
          "env2>filter1.cutoff": {
            "amount": "+2 oct"
          }
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "filters": {
          "filter1": {
            "cutoff": 1400.0
          }
        },
        "routes": {
          "env2>filter1.cutoff": {
            "amount": 2.0
          }
        }
      },
      "description": "Poly. osc1 va saw → svf lowpass 1.40 kHz. env2 (A 100 ms D 400 ms S 0.0 % R 300 ms). env2 → filter1.cutoff +2.00 oct. Amp A 80.0 ms D 300 ms S 70.0 % R 200 ms."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "opened pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-2>",
      "path": "<workdir>/renders/<render-2>.wav",
      "seconds": 2.3000000000000003,
      "peak_dbfs": -16.330000000000002,
      "tail_s": 0.3,
      "tail_capped": true,
      "latency_samples": 0,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -23.1,
          "lra_lu": null,
          "max_momentary_lufs": -22.1,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -16.3,
          "sample_peak_dbfs": -16.3,
          "plr_lu": 6.8
        },
        "spectral": {
          "centroid_hz": 791.0,
          "rolloff_hz": 398.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -5.8,
          "bands": {
            "sub": -47.1,
            "low": -1.6,
            "low_mid": -7.2,
            "mid": -9.4,
            "high_mid": -23.0,
            "presence": -32.7,
            "air": -38.5
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3-1c",
          "stability_cents": 460.6,
          "harmonics_db": [
            0.0,
            -6.2,
            -10.1,
            -13.1,
            -15.6,
            -17.8,
            -19.9,
            -21.8
          ],
          "inharmonicity": 0.0,
          "hnr_db": 47.0
        },
        "envelope": {
          "attack_ms": 63.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.9,
          "crest_db": 9.0,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-23.1 LUFS, true peak -16.3 dBTP. Pitched E3-1c (165 Hz), 2nd/3rd harmonics -6.2/-10.1 dB. Centroid 791 Hz, tilt -5.8 dB/oct, most energy in low. Attack 63 ms, -20 dB after 1020 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "method": "tools/call",
  "params": {
    "name": "compare",
    "arguments": {
      "reference": "<render-1>",
      "test": "<render-2>"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "notable": {
        "_elided_array_count": 18
      },
      "ltas_rms_db": 7.8,
      "summary": "spectral.bands.air.level_db +19.0 dB; spectral.centroid_hz.p90 +739.4 Hz; spectral.bands.presence.level_db +16.5 dB; spectral.bands.high_mid.level_db +11.8 dB; spectral.centroid_hz.mean +352.6 Hz. Timbre difference 7.8 dB (LTAS RMS, level removed).",
      "loss": {
        "total": 80.36952957891427,
        "terms": {
          "bands": 13.927822043692457,
          "centroid": 1.9728050065375413,
          "envelope.attack": 0.04940556564455911,
          "envelope.decay20": 0.0,
          "envelope.release": 0.0,
          "flatness": 1.7709094722703791e-09,
          "flux": 0.0003809826034800333,
          "harmonics": 0.4600659303701549,
          "inharmonicity": 4.935881297447454e-09,
          "ltas": 6.748821628183759,
          "stereo": 0.0,
          "trajectory": 57.210228415175536
        },
        "note_off_s": {
          "reference": 1.9999999999999998,
          "test": 1.9999999999999998
        }
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "tools/call",
  "params": {
    "name": "optimize_start",
    "arguments": {
      "instance": "in_1",
      "params": [
        "filter1.cutoff"
      ],
      "target": {
        "profile": {
          "schema": "supple.analysis-profile/1.0",
          "targets": {
            "spectral.centroid_hz.median": {
              "target": 1500,
              "tolerance": 100
            }
          }
        }
      },
      "phrase": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "evaluations": 20,
      "max_seconds": 120,
      "seed": 2
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "running",
      "params": [
        "filter1.cutoff"
      ],
      "render": "<render-3>"
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "method": "tools/call",
  "params": {
    "name": "exec",
    "arguments": {
      "command": "job.wait",
      "args": {
        "job": "jb_1",
        "timeout_s": 30
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "job": "jb_1",
      "status": "done",
      "progress": {
        "best_loss": 0.2505,
        "evaluations": 17,
        "fraction": 1.0,
        "message": "generation 4"
      },
      "result": {
        "loss": {
          "initial": 50.3315,
          "best": 0.2505,
          "terms": {
            "profile.spectral.centroid_hz.median": 0.2505
          }
        },
        "evaluations": 17,
        "failed": 0,
        "seconds": "<elapsed-job-seconds>",
        "stopped": "evaluations",
        "patch": {
          "params": {
            "filter1.cutoff": 3723.071911
          }
        },
        "changes": [
          {
            "key": "filter1.cutoff",
            "from": 1400.0,
            "to": 3723.0719,
            "unit": "Hz"
          }
        ],
        "render": "<render-3>",
        "applied": false
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "method": "tools/call",
  "params": {
    "name": "patch_merge",
    "arguments": {
      "instance": "in_1",
      "patch": {
        "params": {
          "filter1.cutoff": 3723.071911
        }
      }
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "changed": {
        "filters": {
          "filter1": {
            "cutoff": 3723.071911
          }
        }
      },
      "description": "Poly. osc1 va saw → svf lowpass 3.72 kHz. env2 (A 100 ms D 400 ms S 0.0 % R 300 ms). env2 → filter1.cutoff +2.00 oct. Amp A 80.0 ms D 300 ms S 70.0 % R 200 ms."
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "method": "tools/call",
  "params": {
    "name": "render",
    "arguments": {
      "notation": "1: C3:h E3:q G3:q",
      "seconds": 2.0,
      "tail_s": 0.3,
      "analyze": "summary",
      "name": "optimized pad"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "render": "<render-4>",
      "path": "<workdir>/renders/<render-4>.wav",
      "seconds": 2.3000000000000003,
      "peak_dbfs": -15.83,
      "tail_s": 0.3,
      "tail_capped": true,
      "latency_samples": 0,
      "analysis": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.6,
          "lra_lu": null,
          "max_momentary_lufs": -21.6,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.8,
          "sample_peak_dbfs": -15.8,
          "plr_lu": 6.8
        },
        "spectral": {
          "centroid_hz": 1450.0,
          "rolloff_hz": 492.0,
          "flatness": 0.0,
          "flux": 0.006,
          "tilt_db_per_oct": -4.2,
          "bands": {
            "sub": -47.4,
            "low": -1.9,
            "low_mid": -7.3,
            "mid": -8.4,
            "high_mid": -18.3,
            "presence": -25.4,
            "air": -28.4
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3",
          "stability_cents": 460.9,
          "harmonics_db": [
            0.0,
            -6.1,
            -9.6,
            -12.2,
            -14.2,
            -15.9,
            -17.4,
            -18.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.5,
          "crest_db": 9.2,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0
        },
        "summary": "-22.6 LUFS, true peak -15.8 dBTP. Pitched E3 (165 Hz), 2nd/3rd harmonics -6.1/-9.6 dB. Centroid 1450 Hz, tilt -4.2 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1024 ms. Width 0.00, correlation 1.00."
      }
    }
  }
}
```

### request

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "analyze",
    "arguments": {
      "source": "<render-4>",
      "detail": "standard"
    }
  }
}
```

### response

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "<same as structuredContent>"
      }
    ],
    "structuredContent": {
      "report": {
        "schema": "supple.analysis-report/1.0",
        "source": {
          "seconds": 2.3,
          "sample_rate": 48000.0,
          "channels": 2,
          "layout": "stereo"
        },
        "loudness": {
          "integrated_lufs": -22.6,
          "lra_lu": null,
          "max_momentary_lufs": -21.6,
          "max_short_term_lufs": null,
          "true_peak_dbtp": -15.8,
          "sample_peak_dbfs": -15.8,
          "plr_lu": 6.8
        },
        "spectral": {
          "centroid_hz": {
            "mean": 1650.0,
            "median": 1450.0,
            "p10": 1280.0,
            "p90": 2370.0
          },
          "rolloff_hz": {
            "mean": 501.0,
            "median": 492.0,
            "p10": 398.0,
            "p90": 609.0
          },
          "flatness": {
            "mean": 0.0,
            "median": 0.0,
            "p10": 0.0,
            "p90": 0.001
          },
          "flux": {
            "mean": 0.025,
            "median": 0.006,
            "p10": 0.001,
            "p90": 0.061
          },
          "tilt_db_per_oct": -4.2,
          "bands": {
            "sub": {
              "level_db": -47.4,
              "share_pct": 0.0,
              "width": 0.0
            },
            "low": {
              "level_db": -1.9,
              "share_pct": 64.9,
              "width": 0.0
            },
            "low_mid": {
              "level_db": -7.3,
              "share_pct": 18.8,
              "width": 0.0
            },
            "mid": {
              "level_db": -8.4,
              "share_pct": 14.3,
              "width": 0.0
            },
            "high_mid": {
              "level_db": -18.3,
              "share_pct": 1.5,
              "width": 0.0
            },
            "presence": {
              "level_db": -25.4,
              "share_pct": 0.3,
              "width": 0.0
            },
            "air": {
              "level_db": -28.4,
              "share_pct": 0.1,
              "width": 0.0
            }
          },
          "ltas_db": {
            "_elided_array_count": 31
          }
        },
        "harmonic": {
          "pitched": true,
          "voiced_pct": 97.2,
          "f0_hz": 165.0,
          "note": "E3",
          "stability_cents": 460.9,
          "harmonics_db": [
            0.0,
            -6.1,
            -9.6,
            -12.2,
            -14.2,
            -15.9,
            -17.4,
            -18.7
          ],
          "inharmonicity": 0.0,
          "hnr_db": 46.3
        },
        "envelope": {
          "attack_ms": 68.0,
          "decay20_ms": 1020.0,
          "release_ms": 199.0,
          "sustain_db": -4.5,
          "crest_db": 9.2,
          "onsets_per_s": 0.4
        },
        "stereo": {
          "correlation": 1.0,
          "width": 0.0,
          "balance_db": 0.0,
          "mono_loss_lu": 0.0,
          "correlation_mean": 1.0,
          "correlation_p5": 1.0
        }
      },
      "summary": "-22.6 LUFS, true peak -15.8 dBTP. Pitched E3 (165 Hz), 2nd/3rd harmonics -6.1/-9.6 dB. Centroid 1450 Hz, tilt -4.2 dB/oct, most energy in low. Attack 68 ms, -20 dB after 1024 ms. Width 0.00, correlation 1.00."
    }
  }
}
```

