# synth-reference — hermes / meta/muse-spark-1.3-contributor

Run `20261001T130928234028Z-synth-reference-hermes`; git `f04516c79d368d3e74fd171d1a15caa418d5ae3a`; skill `8ec5d5aa71a779f6b3b316bed27d6af1a85112094d6a2f9c4c56805880b4e6fd`.

Pass rate: 26.7%. Median loss: 3.4402327314710304.

| Task | Repeat | Valid | Pass | Loss | Calls | Seconds |
|---|---:|---|---|---:|---:|---:|
| reference.acid-bass | 1 | True | False | 1.3605873897725038 | 43 | 610.45 |
| reference.acid-bass | 2 | True | False | 2.554254021955823 | 49 | 610.14 |
| reference.acid-bass | 3 | True | False | 2.8602823201433734 | 57 | 607.99 |
| reference.bell | 1 | True | False | 4.353432188358122 | 51 | 608.95 |
| reference.bell | 2 | True | False | 5.2879110941599485 | 50 | 610.29 |
| reference.bell | 3 | True | False | 21.401181932588347 | 67 | 612.50 |
| reference.fm-e-piano | 1 | True | False | 19.57111618858557 | 39 | 609.33 |
| reference.fm-e-piano | 2 | True | False | 4.389719773422053 | 57 | 609.05 |
| reference.fm-e-piano | 3 | True | False | 15.688479105022248 | 62 | 608.23 |
| reference.formant-lead | 1 | True | False | 18.148339593430535 | 47 | 520.86 |
| reference.formant-lead | 2 | True | False | 16.50176753993705 | 48 | 608.89 |
| reference.formant-lead | 3 | True | False | 34.07332028317388 | 60 | 608.33 |
| reference.noise-snare | 1 | True | True | 0.9191984674746365 | 16 | 250.47 |
| reference.noise-snare | 2 | True | True | 0.37218100226846984 | 19 | 216.69 |
| reference.noise-snare | 3 | True | True | 0.6168063173749364 | 18 | 189.22 |
| reference.pad | 1 | True | False | 3.6459775651875153 | 44 | 609.67 |
| reference.pad | 2 | True | False | 3.5695743354317995 | 45 | 611.36 |
| reference.pad | 3 | True | False | 3.342382557086666 | 52 | 619.48 |
| reference.pluck-bass | 1 | True | True | 0.17266294335172838 | 14 | 188.98 |
| reference.pluck-bass | 2 | True | True | 0.4730129370286861 | 13 | 124.25 |
| reference.pluck-bass | 3 | True | True | 0.629790130023001 | 26 | 227.13 |
| reference.strings | 1 | True | False | 3.6775069923959283 | 41 | 609.67 |
| reference.strings | 2 | True | False | 5.731539879359907 | 51 | 605.04 |
| reference.strings | 3 | True | False | 3.558083167385259 | 47 | 607.86 |
| reference.sub-bass | 1 | True | False | 2.0034298753240596 | 63 | 609.83 |
| reference.sub-bass | 2 | True | False | 3.0612999721796577 | 29 | 315.51 |
| reference.sub-bass | 3 | True | False | 3.538082905855395 | 32 | 319.55 |
| reference.supersaw-lead | 1 | True | False | 1.056198866868168 | 38 | 605.76 |
| reference.supersaw-lead | 2 | True | True | 0.8340652111376125 | 34 | 504.61 |
| reference.supersaw-lead | 3 | True | True | 0.8076311453870797 | 33 | 465.68 |

Usage: `{"total_tokens": 12792303, "estimated_cost_usd": 0.8804516099999999, "cost_known": false}`.

- `reference.acid-bass`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.acid-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.acid-bass-worst.wav` — Is the resonant snap recognizably acid-like and controlled?
- `reference.bell`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.bell-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.bell-worst.wav` — Does the strike sound metallic, with a smooth ringing decay?
- `reference.fm-e-piano`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.fm-e-piano-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.fm-e-piano-worst.wav` — Does the attack have FM character while the body stays mellow?
- `reference.formant-lead`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.formant-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.formant-lead-worst.wav` — Can you hear vowel-like movement without losing the pitched lead?
- `reference.noise-snare`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.noise-snare-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.noise-snare-worst.wav` — Is the body audible under the fizz, and does the hit end cleanly?
- `reference.pad`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.pad-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.pad-worst.wav` — Does it swell warmly and stay wide without sounding harsh?
- `reference.pluck-bass`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.pluck-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.pluck-bass-worst.wav` — Is the filter snap short and audible, with convincing bass weight?
- `reference.strings`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.strings-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.strings-worst.wav` — Does it feel like a cohesive sustained ensemble with a natural swell?
- `reference.sub-bass`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.sub-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.sub-bass-worst.wav` — Is the sub clean and solid, free of clicks or harsh upper harmonics?
- `reference.supersaw-lead`: `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.supersaw-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T130928234028Z-synth-reference-hermes/reference.supersaw-lead-worst.wav` — Is this a bright, playable lead with a stable wide image?
