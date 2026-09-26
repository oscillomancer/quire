# Generated reference

Everything in this directory is produced from the code and must never be
edited by hand:

- `commands.md` — from the command registry (`<slug>-sandbox registry --markdown`,
  later the engine's registry).
- `plugins/<plugin>.md` — from each stock plugin's semantic schema.
- `notation-reference.md` — from `docs/specs/notation.md` Part B plus the
  drum map and chord tables compiled into `libs/notation`.

A test regenerates these files and fails when they differ from what is
committed, so the skill can never drift from the installed version.
