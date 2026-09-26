# Generated reference

Everything in this directory is produced from the code and must never be
edited by hand:

- `sandbox-commands.md` — the sandbox's command reference
  (`<slug>-sandbox registry --markdown`); the engine's `commands.md` follows.
- `sandbox-mcp-tools.json` — the MCP tool definitions the sandbox serves
  (`<slug>-sandbox registry --mcp-tools`).

Regenerate with `tools/gen/update-generated.sh`.
- `plugins/<plugin>.md` — from each stock plugin's semantic schema.
- `notation-reference.md` — from `docs/specs/notation.md` Part B plus the
  drum map and chord tables compiled into `libs/notation`.

A test regenerates these files and fails when they differ from what is
committed, so the skill can never drift from the installed version.
