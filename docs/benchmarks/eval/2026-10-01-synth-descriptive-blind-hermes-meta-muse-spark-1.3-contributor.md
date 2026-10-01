# synth-descriptive-blind — hermes / meta/muse-spark-1.3-contributor

Run `20261001T035104417802Z-synth-descriptive-hermes`; git `587f9e2dc8ff4a18435d8087de154471cc739025`; skill `20f78dfab8de9bf4a2c14556a2bb73456c79966e946bd42a7a6b1b9b7e359421`.

Pass rate: 83.3%. Median loss: 0.0.

| Task | Repeat | Valid | Pass | Loss | Calls | Seconds |
|---|---:|---|---|---:|---:|---:|
| synth.acid-bass | 1 | True | True | 0.0 | 5 | 48.70 |
| synth.acid-bass | 2 | True | True | 0.0 | 6 | 41.86 |
| synth.acid-bass | 3 | True | True | 0.0 | 5 | 44.35 |
| synth.bell | 1 | True | True | 0.0 | 23 | 188.75 |
| synth.bell | 2 | True | True | 0.0 | 22 | 157.16 |
| synth.bell | 3 | True | True | 0.0 | 16 | 468.04 |
| synth.fm-e-piano | 1 | True | True | 0.0 | 14 | 122.08 |
| synth.fm-e-piano | 2 | True | True | 0.0 | 8 | 77.45 |
| synth.fm-e-piano | 3 | True | True | 0.0 | 10 | 80.98 |
| synth.formant-lead | 1 | True | True | 0.0 | 10 | 76.97 |
| synth.formant-lead | 2 | True | False | 0.0004 | 6 | 64.89 |
| synth.formant-lead | 3 | True | True | 0.0 | 6 | 37.71 |
| synth.noise-snare | 1 | True | True | 0.0 | 12 | 85.90 |
| synth.noise-snare | 2 | True | False | 8.099999999999999e-05 | 29 | 194.22 |
| synth.noise-snare | 3 | True | False | 0.038809 | 19 | 156.78 |
| synth.pad | 1 | True | True | 0.0 | 8 | 96.88 |
| synth.pad | 2 | True | True | 0.0 | 8 | 70.74 |
| synth.pad | 3 | True | True | 0.0 | 30 | 197.52 |
| synth.pluck-bass | 1 | True | True | 0.0 | 11 | 165.36 |
| synth.pluck-bass | 2 | True | True | 0.0 | 31 | 281.09 |
| synth.pluck-bass | 3 | True | True | 0.0 | 7 | 76.87 |
| synth.strings | 1 | True | False | 0.6740409999999999 | 17 | 102.21 |
| synth.strings | 2 | True | False | 0.016900000000000002 | 16 | 107.55 |
| synth.strings | 3 | True | True | 0.0 | 9 | 59.20 |
| synth.sub-bass | 1 | True | True | 0.0 | 3 | 31.07 |
| synth.sub-bass | 2 | True | True | 0.0 | 6 | 46.97 |
| synth.sub-bass | 3 | True | True | 0.0 | 5 | 46.07 |
| synth.supersaw-lead | 1 | True | True | 0.0 | 16 | 96.04 |
| synth.supersaw-lead | 2 | True | True | 0.0 | 15 | 157.58 |
| synth.supersaw-lead | 3 | True | True | 0.0 | 29 | 279.86 |

Usage: `{"total_tokens": 17830751, "estimated_cost_usd": 0.8175877720000001, "cost_known": true}`.

- `synth.acid-bass`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.acid-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.acid-bass-worst.wav` — Is the resonant snap recognizably acid-like and controlled?
- `synth.bell`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.bell-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.bell-worst.wav` — Does the strike sound metallic, with a smooth ringing decay?
- `synth.fm-e-piano`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.fm-e-piano-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.fm-e-piano-worst.wav` — Does the attack have FM character while the body stays mellow?
- `synth.formant-lead`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.formant-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.formant-lead-worst.wav` — Can you hear vowel-like movement without losing the pitched lead?
- `synth.noise-snare`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.noise-snare-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.noise-snare-worst.wav` — Is the body audible under the fizz, and does the hit end cleanly?
- `synth.pad`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.pad-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.pad-worst.wav` — Does it swell warmly and stay wide without sounding harsh?
- `synth.pluck-bass`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.pluck-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.pluck-bass-worst.wav` — Is the filter snap short and audible, with convincing bass weight?
- `synth.strings`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.strings-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.strings-worst.wav` — Does it feel like a cohesive sustained ensemble with a natural swell?
- `synth.sub-bass`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.sub-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.sub-bass-worst.wav` — Is the sub clean and solid, free of clicks or harsh upper harmonics?
- `synth.supersaw-lead`: `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.supersaw-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T035104417802Z-synth-descriptive-hermes/synth.supersaw-lead-worst.wav` — Is this a bright, playable lead with a stable wide image?
