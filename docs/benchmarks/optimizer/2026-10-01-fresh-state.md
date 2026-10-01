# Optimizer reference-search benchmark — 2026-10-01-fresh-state

Orrery, 300 evaluations per run, seeds 1, 2, 3, patch seed 42, 2 modes.

Not applicable: reference.formant-lead, reference.noise-snare, reference.strings (tier 3 starts from an empty patch — a structure task).

| Mode | Runs | Median best | Min | Max | Pass (≤ max_loss) | Median evals | Median s | Skipped | Failed candidates | Dim |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `auto` | 21 | 3.3084 | 0.616 | 66.4043 | 3/21 | 300 | 35.4 | 0 | 0 | — |
| `module` | 21 | 1.0986 | 0.0023 | 39.6121 | 10/21 | 297 | 30.6 | 0 | 0 | 5, 13, 14 |

Per task and seed (`best` is the loss the optimizer stopped at; the reference is the task's own hidden patch):

| Task | Tier | Mode | Dim | Seed | Initial | Best | Pass | Evals | Seconds | Stopped | Failed |
|---|---:|---|---:|---:|---:|---:|:--:|---:|---:|---|---:|
| reference.acid-bass | 2 | `auto` | None | 1 | 25.3467 | 4.4701 | no | 300 | 30.68 | evaluations | 0 |
| reference.acid-bass | 2 | `auto` | None | 2 | 25.3467 | 4.0178 | no | 300 | 27.94 | evaluations | 0 |
| reference.acid-bass | 2 | `auto` | None | 3 | 25.3467 | 2.3655 | no | 300 | 30.55 | evaluations | 0 |
| reference.acid-bass | 2 | `module` | 13 | 1 | 25.3467 | 3.0314 | no | 298 | 27.61 | evaluations | 0 |
| reference.acid-bass | 2 | `module` | 13 | 2 | 25.3467 | 4.1047 | no | 298 | 26.98 | evaluations | 0 |
| reference.acid-bass | 2 | `module` | 13 | 3 | 25.3467 | 1.0986 | no | 298 | 26.69 | evaluations | 0 |
| reference.bell | 2 | `auto` | None | 1 | 39.7556 | 29.4267 | no | 300 | 33.64 | evaluations | 0 |
| reference.bell | 2 | `auto` | None | 2 | 39.7556 | 21.9527 | no | 300 | 33.71 | evaluations | 0 |
| reference.bell | 2 | `auto` | None | 3 | 39.7556 | 24.9863 | no | 300 | 35.44 | evaluations | 0 |
| reference.bell | 2 | `module` | 14 | 1 | 39.7556 | 24.6831 | no | 298 | 29.38 | evaluations | 0 |
| reference.bell | 2 | `module` | 14 | 2 | 39.7556 | 39.6121 | no | 298 | 25.33 | evaluations | 0 |
| reference.bell | 2 | `module` | 14 | 3 | 39.7556 | 39.0989 | no | 298 | 30.58 | evaluations | 0 |
| reference.fm-e-piano | 2 | `auto` | None | 1 | 421.2685 | 51.7362 | no | 300 | 30.46 | evaluations | 0 |
| reference.fm-e-piano | 2 | `auto` | None | 2 | 421.2685 | 20.0464 | no | 300 | 28.63 | evaluations | 0 |
| reference.fm-e-piano | 2 | `auto` | None | 3 | 421.2685 | 66.4043 | no | 300 | 30.31 | evaluations | 0 |
| reference.fm-e-piano | 2 | `module` | 14 | 1 | 421.2685 | 29.8679 | no | 298 | 29.72 | evaluations | 0 |
| reference.fm-e-piano | 2 | `module` | 14 | 2 | 421.2685 | 24.741 | no | 298 | 28.5 | evaluations | 0 |
| reference.fm-e-piano | 2 | `module` | 14 | 3 | 421.2685 | 14.7504 | no | 298 | 26.14 | evaluations | 0 |
| reference.pad | 1 | `auto` | None | 1 | 25.7811 | 5.7436 | no | 295 | 74.59 | evaluations | 0 |
| reference.pad | 1 | `auto` | None | 2 | 25.7811 | 3.2591 | no | 295 | 73.91 | evaluations | 0 |
| reference.pad | 1 | `auto` | None | 3 | 25.7811 | 3.3812 | no | 295 | 86.88 | evaluations | 0 |
| reference.pad | 1 | `module` | 5 | 1 | 25.7811 | 0.0116 | yes | 297 | 108.61 | evaluations | 0 |
| reference.pad | 1 | `module` | 5 | 2 | 25.7811 | 0.0054 | yes | 297 | 94.62 | evaluations | 0 |
| reference.pad | 1 | `module` | 5 | 3 | 25.7811 | 0.0023 | yes | 297 | 121.5 | evaluations | 0 |
| reference.pluck-bass | 1 | `auto` | None | 1 | 29.7355 | 0.985 | yes | 289 | 26.9 | evaluations | 0 |
| reference.pluck-bass | 1 | `auto` | None | 2 | 29.7355 | 0.616 | yes | 289 | 47.8 | evaluations | 0 |
| reference.pluck-bass | 1 | `auto` | None | 3 | 29.7355 | 0.7763 | yes | 289 | 48.94 | evaluations | 0 |
| reference.pluck-bass | 1 | `module` | 5 | 1 | 29.7355 | 0.0178 | yes | 297 | 35.69 | evaluations | 0 |
| reference.pluck-bass | 1 | `module` | 5 | 2 | 29.7355 | 0.0216 | yes | 297 | 30.58 | evaluations | 0 |
| reference.pluck-bass | 1 | `module` | 5 | 3 | 29.7355 | 0.027 | yes | 297 | 39.44 | evaluations | 0 |
| reference.sub-bass | 1 | `auto` | None | 1 | 4.0139 | 2.765 | no | 291 | 26.38 | evaluations | 0 |
| reference.sub-bass | 1 | `auto` | None | 2 | 4.0139 | 2.8136 | no | 291 | 52.77 | evaluations | 0 |
| reference.sub-bass | 1 | `auto` | None | 3 | 4.0139 | 3.3084 | no | 291 | 42.01 | evaluations | 0 |
| reference.sub-bass | 1 | `module` | 5 | 1 | 4.0139 | 3.324 | no | 297 | 33.26 | evaluations | 0 |
| reference.sub-bass | 1 | `module` | 5 | 2 | 4.0139 | 3.0065 | no | 297 | 28.53 | evaluations | 0 |
| reference.sub-bass | 1 | `module` | 5 | 3 | 4.0139 | 0.0485 | yes | 297 | 33.77 | evaluations | 0 |
| reference.supersaw-lead | 1 | `auto` | None | 1 | 16.2888 | 1.2056 | no | 300 | 78.66 | evaluations | 0 |
| reference.supersaw-lead | 1 | `auto` | None | 2 | 16.2888 | 1.9315 | no | 300 | 62.81 | evaluations | 0 |
| reference.supersaw-lead | 1 | `auto` | None | 3 | 16.2888 | 2.1787 | no | 300 | 54.71 | evaluations | 0 |
| reference.supersaw-lead | 1 | `module` | 5 | 1 | 16.2888 | 0.026 | yes | 297 | 65.27 | evaluations | 0 |
| reference.supersaw-lead | 1 | `module` | 5 | 2 | 16.2888 | 0.0894 | yes | 297 | 79.23 | evaluations | 0 |
| reference.supersaw-lead | 1 | `module` | 5 | 3 | 16.2888 | 0.025 | yes | 297 | 56.75 | evaluations | 0 |
