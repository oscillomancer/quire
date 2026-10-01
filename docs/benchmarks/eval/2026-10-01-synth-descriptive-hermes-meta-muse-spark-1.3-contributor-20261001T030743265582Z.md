# synth-descriptive — hermes / meta/muse-spark-1.3-contributor

Run `20261001T030743265582Z-synth-descriptive-hermes`; git `065299898606d5e621161ad5ba2d18caf76cdf13`; skill `20f78dfab8de9bf4a2c14556a2bb73456c79966e946bd42a7a6b1b9b7e359421`.

Pass rate: 100.0%. Median loss: 0.0.

| Task | Repeat | Valid | Pass | Loss | Calls | Seconds |
|---|---:|---|---|---:|---:|---:|
| synth.acid-bass | 1 | True | True | 0.0 | 6 | 36.53 |
| synth.acid-bass | 2 | True | True | 0.0 | 3 | 39.11 |
| synth.acid-bass | 3 | True | True | 0.0 | 6 | 56.81 |
| synth.bell | 1 | True | True | 0.0 | 10 | 124.80 |
| synth.bell | 2 | True | True | 0.0 | 16 | 82.91 |
| synth.bell | 3 | True | True | 0.0 | 10 | 89.12 |
| synth.fm-e-piano | 1 | True | True | 0.0 | 15 | 171.47 |
| synth.fm-e-piano | 2 | True | True | 0.0 | 10 | 100.16 |
| synth.fm-e-piano | 3 | True | True | 0.0 | 5 | 49.82 |
| synth.formant-lead | 1 | True | True | 0.0 | 6 | 56.01 |
| synth.formant-lead | 2 | True | True | 0.0 | 6 | 46.65 |
| synth.formant-lead | 3 | True | True | 0.0 | 7 | 75.01 |
| synth.noise-snare | 1 | True | True | 0.0 | 14 | 113.97 |
| synth.noise-snare | 2 | True | True | 0.0 | 14 | 133.44 |
| synth.noise-snare | 3 | True | True | 0.0 | 5 | 82.16 |
| synth.pad | 1 | True | True | 0.0 | 7 | 72.89 |
| synth.pad | 2 | True | True | 0.0 | 5 | 66.81 |
| synth.pad | 3 | True | True | 0.0 | 5 | 57.77 |
| synth.pluck-bass | 1 | True | True | 0.0 | 6 | 67.01 |
| synth.pluck-bass | 2 | True | True | 0.0 | 10 | 117.67 |
| synth.pluck-bass | 3 | True | True | 0.0 | 17 | 306.40 |
| synth.strings | 1 | True | True | 0.0 | 4 | 75.29 |
| synth.strings | 2 | True | True | 0.0 | 7 | 57.76 |
| synth.strings | 3 | True | True | 0.0 | 8 | 63.48 |
| synth.sub-bass | 1 | True | True | 0.0 | 6 | 71.01 |
| synth.sub-bass | 2 | True | True | 0.0 | 8 | 107.23 |
| synth.sub-bass | 3 | True | True | 0.0 | 8 | 50.55 |
| synth.supersaw-lead | 1 | True | True | 0.0 | 6 | 53.91 |
| synth.supersaw-lead | 2 | True | True | 0.0 | 6 | 52.50 |
| synth.supersaw-lead | 3 | True | True | 0.0 | 5 | 61.20 |

Usage: `{"total_tokens": 8922183, "estimated_cost_usd": 0.549868472, "cost_known": true}`.

- `synth.acid-bass`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.acid-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.acid-bass-worst.wav` — Is the resonant snap recognizably acid-like and controlled?
- `synth.bell`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.bell-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.bell-worst.wav` — Does the strike sound metallic, with a smooth ringing decay?
- `synth.fm-e-piano`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.fm-e-piano-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.fm-e-piano-worst.wav` — Does the attack have FM character while the body stays mellow?
- `synth.formant-lead`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.formant-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.formant-lead-worst.wav` — Can you hear vowel-like movement without losing the pitched lead?
- `synth.noise-snare`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.noise-snare-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.noise-snare-worst.wav` — Is the body audible under the fizz, and does the hit end cleanly?
- `synth.pad`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.pad-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.pad-worst.wav` — Does it swell warmly and stay wide without sounding harsh?
- `synth.pluck-bass`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.pluck-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.pluck-bass-worst.wav` — Is the filter snap short and audible, with convincing bass weight?
- `synth.strings`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.strings-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.strings-worst.wav` — Does it feel like a cohesive sustained ensemble with a natural swell?
- `synth.sub-bass`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.sub-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.sub-bass-worst.wav` — Is the sub clean and solid, free of clicks or harsh upper harmonics?
- `synth.supersaw-lead`: `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.supersaw-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T030743265582Z-synth-descriptive-hermes/synth.supersaw-lead-worst.wav` — Is this a bright, playable lead with a stable wide image?
