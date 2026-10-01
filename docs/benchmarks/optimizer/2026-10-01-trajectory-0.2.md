# Optimizer reference-search benchmark — 2026-10-01-trajectory-0.2

Orrery, 300 evaluations per run, seeds 1, 2, 3, patch seed 42, 2 modes.

Not applicable: reference.formant-lead, reference.noise-snare, reference.strings (tier 3 starts from an empty patch — a structure task).

| Mode | Runs | Median best | Min | Max | Pass (≤ max_loss) | Median evals | Median s | Skipped | Failed candidates | Dim |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `auto` | 21 | 1.9187 | 0.1609 | 68.4007 | 6/21 | 300 | 47.8 | 0 | 0 | — |
| `module` | 21 | 1.4334 | 0.0027 | 28.5826 | 10/21 | 297 | 50.9 | 0 | 0 | 5, 13, 14 |

Per task and seed (`best` is the loss the optimizer stopped at; the reference is the task's own hidden patch):

| Task | Tier | Mode | Dim | Seed | Initial | Best | Pass | Evals | Seconds | Stopped | Failed |
|---|---:|---|---:|---:|---:|---:|:--:|---:|---:|---|---:|
| reference.acid-bass | 2 | `auto` | None | 1 | 16.6098 | 5.3818 | no | 300 | 61.22 | evaluations | 0 |
| reference.acid-bass | 2 | `auto` | None | 2 | 16.6098 | 5.725 | no | 300 | 45.63 | evaluations | 0 |
| reference.acid-bass | 2 | `auto` | None | 3 | 16.6098 | 2.1112 | no | 300 | 32.76 | evaluations | 0 |
| reference.acid-bass | 2 | `module` | 13 | 1 | 16.6098 | 1.4334 | no | 298 | 42.08 | evaluations | 0 |
| reference.acid-bass | 2 | `module` | 13 | 2 | 16.6098 | 1.5221 | no | 298 | 44.67 | evaluations | 0 |
| reference.acid-bass | 2 | `module` | 13 | 3 | 16.6098 | 2.4226 | no | 298 | 37.98 | evaluations | 0 |
| reference.bell | 2 | `auto` | None | 1 | 32.938 | 10.9715 | no | 300 | 67.14 | evaluations | 0 |
| reference.bell | 2 | `auto` | None | 2 | 32.938 | 16.4751 | no | 300 | 49.16 | evaluations | 0 |
| reference.bell | 2 | `auto` | None | 3 | 32.938 | 13.8786 | no | 300 | 44.47 | evaluations | 0 |
| reference.bell | 2 | `module` | 14 | 1 | 32.938 | 28.5826 | no | 298 | 47.77 | evaluations | 0 |
| reference.bell | 2 | `module` | 14 | 2 | 32.938 | 25.4816 | no | 298 | 43.95 | evaluations | 0 |
| reference.bell | 2 | `module` | 14 | 3 | 32.938 | 24.8004 | no | 298 | 52.87 | evaluations | 0 |
| reference.fm-e-piano | 2 | `auto` | None | 1 | 150.5648 | 68.4007 | no | 300 | 63.89 | evaluations | 0 |
| reference.fm-e-piano | 2 | `auto` | None | 2 | 150.5648 | 28.7948 | no | 300 | 45.19 | evaluations | 0 |
| reference.fm-e-piano | 2 | `auto` | None | 3 | 150.5648 | 14.2445 | no | 300 | 36.25 | evaluations | 0 |
| reference.fm-e-piano | 2 | `module` | 14 | 1 | 150.5648 | 15.9866 | no | 298 | 43.92 | evaluations | 0 |
| reference.fm-e-piano | 2 | `module` | 14 | 2 | 150.5648 | 16.3848 | no | 298 | 47.25 | evaluations | 0 |
| reference.fm-e-piano | 2 | `module` | 14 | 3 | 150.5648 | 7.9778 | no | 298 | 38.99 | evaluations | 0 |
| reference.pad | 1 | `auto` | None | 1 | 11.8241 | 2.9155 | no | 295 | 120.98 | evaluations | 0 |
| reference.pad | 1 | `auto` | None | 2 | 11.8241 | 1.3673 | no | 295 | 104.91 | evaluations | 0 |
| reference.pad | 1 | `auto` | None | 3 | 11.8241 | 1.2594 | no | 295 | 101.69 | evaluations | 0 |
| reference.pad | 1 | `module` | 5 | 1 | 11.8241 | 0.0139 | yes | 297 | 126.5 | evaluations | 0 |
| reference.pad | 1 | `module` | 5 | 2 | 11.8241 | 0.0062 | yes | 297 | 142.74 | evaluations | 0 |
| reference.pad | 1 | `module` | 5 | 3 | 11.8241 | 0.0027 | yes | 297 | 106.14 | evaluations | 0 |
| reference.pluck-bass | 1 | `auto` | None | 1 | 14.6374 | 0.3232 | yes | 289 | 38.11 | evaluations | 0 |
| reference.pluck-bass | 1 | `auto` | None | 2 | 14.6374 | 0.1609 | yes | 289 | 37.9 | evaluations | 0 |
| reference.pluck-bass | 1 | `auto` | None | 3 | 14.6374 | 0.2609 | yes | 289 | 47.8 | evaluations | 0 |
| reference.pluck-bass | 1 | `module` | 5 | 1 | 14.6374 | 0.0223 | yes | 297 | 50.93 | evaluations | 0 |
| reference.pluck-bass | 1 | `module` | 5 | 2 | 14.6374 | 0.0519 | yes | 257 | 50.36 | stagnation | 0 |
| reference.pluck-bass | 1 | `module` | 5 | 3 | 14.6374 | 0.0373 | yes | 297 | 63.23 | evaluations | 0 |
| reference.sub-bass | 1 | `auto` | None | 1 | 1.9189 | 1.9187 | no | 291 | 34.42 | evaluations | 0 |
| reference.sub-bass | 1 | `auto` | None | 2 | 1.9189 | 1.9187 | no | 291 | 36.95 | evaluations | 0 |
| reference.sub-bass | 1 | `auto` | None | 3 | 1.9189 | 1.9187 | no | 291 | 44.67 | evaluations | 0 |
| reference.sub-bass | 1 | `module` | 5 | 1 | 1.9189 | 1.9187 | no | 281 | 45.73 | stagnation | 0 |
| reference.sub-bass | 1 | `module` | 5 | 2 | 1.9189 | 1.9187 | no | 297 | 52.46 | evaluations | 0 |
| reference.sub-bass | 1 | `module` | 5 | 3 | 1.9189 | 0.0158 | yes | 297 | 55.18 | evaluations | 0 |
| reference.supersaw-lead | 1 | `auto` | None | 1 | 6.0955 | 0.5838 | yes | 300 | 70.88 | evaluations | 0 |
| reference.supersaw-lead | 1 | `auto` | None | 2 | 6.0955 | 0.6148 | yes | 300 | 87.29 | evaluations | 0 |
| reference.supersaw-lead | 1 | `auto` | None | 3 | 6.0955 | 0.5356 | yes | 300 | 99.1 | evaluations | 0 |
| reference.supersaw-lead | 1 | `module` | 5 | 1 | 6.0955 | 0.018 | yes | 297 | 69.88 | evaluations | 0 |
| reference.supersaw-lead | 1 | `module` | 5 | 2 | 6.0955 | 0.0233 | yes | 297 | 63.35 | evaluations | 0 |
| reference.supersaw-lead | 1 | `module` | 5 | 3 | 6.0955 | 0.0172 | yes | 297 | 81.37 | evaluations | 0 |
