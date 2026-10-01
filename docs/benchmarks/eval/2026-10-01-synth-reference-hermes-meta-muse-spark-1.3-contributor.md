# synth-reference — hermes / meta/muse-spark-1.3-contributor

Run `20261001T034021918752Z-synth-reference-hermes`; git `b47bac64056f97e2be293689ac684d6888988099`; skill `20f78dfab8de9bf4a2c14556a2bb73456c79966e946bd42a7a6b1b9b7e359421`.

Pass rate: 30.0%. Median loss: 3.9939974826939273.

| Task | Repeat | Valid | Pass | Loss | Calls | Seconds |
|---|---:|---|---|---:|---:|---:|
| reference.acid-bass | 1 | True | False | 7.131637843924262 | 36 | 339.35 |
| reference.acid-bass | 2 | True | False | 1.8861362058346063 | 56 | 522.42 |
| reference.acid-bass | 3 | True | False | 5879.287724564669 | 26 | 345.89 |
| reference.bell | 1 | True | False | 14.698197294076786 | 64 | 604.89 |
| reference.bell | 2 | True | False | 17.91869506787844 | 47 | 522.09 |
| reference.bell | 3 | True | False | 3.2282625704679564 | 65 | 500.73 |
| reference.fm-e-piano | 1 | True | False | 5.477378246671398 | 35 | 334.17 |
| reference.fm-e-piano | 2 | True | False | 101.68058044830147 | 71 | 604.68 |
| reference.fm-e-piano | 3 | True | False | 21.290276150899448 | 28 | 257.34 |
| reference.formant-lead | 1 | True | False | 23.30031265893547 | 58 | 604.77 |
| reference.formant-lead | 2 | True | False | 16.41937730010077 | 51 | 604.35 |
| reference.formant-lead | 3 | True | False | 25.86722193214646 | 81 | 509.44 |
| reference.noise-snare | 1 | True | True | 0.9189882198818938 | 48 | 433.43 |
| reference.noise-snare | 2 | True | True | 0.29560337241516743 | 49 | 373.08 |
| reference.noise-snare | 3 | True | True | 0.34336725110619826 | 37 | 292.29 |
| reference.pad | 1 | True | True | 0.5418296885869984 | 37 | 382.88 |
| reference.pad | 2 | True | False | 47.406024452020425 | 56 | 569.31 |
| reference.pad | 3 | True | False | 1.1609161597236939 | 18 | 225.43 |
| reference.pluck-bass | 1 | True | True | 0.6978088460187332 | 30 | 246.50 |
| reference.pluck-bass | 2 | True | True | 0.07418929151480662 | 35 | 259.38 |
| reference.pluck-bass | 3 | True | True | 0.13963627186446356 | 39 | 244.90 |
| reference.strings | 1 | True | False | 2.7430809913514502 | 41 | 385.73 |
| reference.strings | 2 | True | False | 5.784885519418783 | 30 | 259.68 |
| reference.strings | 3 | True | False | 5.124762947461308 | 33 | 386.45 |
| reference.sub-bass | 1 | True | False | 3.9939974826939273 | 13 | 175.13 |
| reference.sub-bass | 2 | True | False | 3.9939974826939273 | 15 | 216.46 |
| reference.sub-bass | 3 | True | False | 3.9939974826939273 | 15 | 250.24 |
| reference.supersaw-lead | 1 | True | True | 0.017923329981175684 | 31 | 290.32 |
| reference.supersaw-lead | 2 | True | True | 0.0008873482516723137 | 24 | 238.09 |
| reference.supersaw-lead | 3 | True | False | 4.762436621074843 | 22 | 604.53 |

Usage: `{"total_tokens": 43988893, "estimated_cost_usd": 1.9492292759999996, "cost_known": false}`.

- `reference.acid-bass`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.acid-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.acid-bass-worst.wav` — Is the resonant snap recognizably acid-like and controlled?
- `reference.bell`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.bell-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.bell-worst.wav` — Does the strike sound metallic, with a smooth ringing decay?
- `reference.fm-e-piano`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.fm-e-piano-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.fm-e-piano-worst.wav` — Does the attack have FM character while the body stays mellow?
- `reference.formant-lead`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.formant-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.formant-lead-worst.wav` — Can you hear vowel-like movement without losing the pitched lead?
- `reference.noise-snare`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.noise-snare-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.noise-snare-worst.wav` — Is the body audible under the fizz, and does the hit end cleanly?
- `reference.pad`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.pad-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.pad-worst.wav` — Does it swell warmly and stay wide without sounding harsh?
- `reference.pluck-bass`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.pluck-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.pluck-bass-worst.wav` — Is the filter snap short and audible, with convincing bass weight?
- `reference.strings`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.strings-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.strings-worst.wav` — Does it feel like a cohesive sustained ensemble with a natural swell?
- `reference.sub-bass`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.sub-bass-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.sub-bass-worst.wav` — Is the sub clean and solid, free of clicks or harsh upper harmonics?
- `reference.supersaw-lead`: `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.supersaw-lead-best.wav`, `docs/benchmarks/eval/audio/20261001T034021918752Z-synth-reference-hermes/reference.supersaw-lead-worst.wav` — Is this a bright, playable lead with a stable wide image?
