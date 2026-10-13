# Generated reference

Everything in this directory is produced from the code and must never be
edited by hand:

- `sandbox-commands.md` — the sandbox's command reference
  (`<slug>-sandbox registry --markdown`); the engine's is `engine-commands.md`.
- `sandbox-mcp-tools.json` — the MCP tool definitions the sandbox serves
  (`<slug>-sandbox registry --mcp-tools`).
- `engine-commands.md`, `engine-mcp-tools.json` — the same for the headless
  DAW (`<slug>-engine registry …`).
- `engine.lua`, `sandbox.lua` — LuaLS annotations of the scripting bindings
  (`<slug>-engine registry --lua`): the `q` table of a script, generated from
  the same schemas (`guides/lua.md`).

Regenerate with `tools/gen/update-generated.sh`.
- `plugins/<plugin>.md` — from each stock plugin's semantic schema.
- `notation-reference.md` — from the syntax and tables compiled into
  `libs/notation`, with examples compiled and decompiled by the generator.
- `dsp-examples.md` — the example DSP scripts (`plugins/suite/examples`),
  each tested, with a table of what each one is.

A test regenerates these files and fails when they differ from what is
committed, so the skill can never drift from the installed version.
