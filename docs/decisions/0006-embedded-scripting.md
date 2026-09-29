# ADR-0006: Embedded scripting language

**Status:** Accepted, 2026-09-26.

## Context

The DAW needs an embedded scripting language for user scripts, custom
actions, and agent-written automation (build prompt 2.4: Lua is the default
candidate). Scripts must use the same command bus as everything else, must
never touch the audio thread, and must be unable to take the session down.
Phase 7 adds LSP definitions, hot reload and an example library. (DSP
scripting is a separate, JIT-compiled language: Phase 7, its own ADR.)

## Decision

**PUC Lua 5.4** (MIT), vendored and statically linked, pinned to the latest
5.4.x release when the Lua binding work starts (Phase 4).

- Scripts run on the model thread (or a script worker that submits to it),
  never on the audio thread.
- Each script gets a sandboxed environment: `string`, `table`, `math`,
  `utf8`, `coroutine`; no `io`, `os`, `debug`, `package`/`require` from disk
  except a whitelisted script-library path; an instruction-count hook enforces
  a time budget; a custom allocator enforces a memory cap.
- Bindings are **generated from the command registry**: `q.track.add{name =
  "Bass"}` returns the result as a table or raises a structured error table
  (`{code, message, path, hint}`); `q.batch("label", function() ... end)`
  runs a transaction; `q.on("session.changed", fn, filter)` subscribes to
  events. There is no hand-written binding surface to drift.
- Generated LuaLS (`---@class`/`---@param`) annotation files give editor
  autocomplete and type checking outside the DAW.

## Implementation (Phase 4, 2026-09-28)

- The language is vendored as **5.4.9** (lua.org tarball, MIT) under
  `third_party/lua-5.4.9` and built as a static library with only the core and
  the sandbox's libraries: `io`, `os`, `debug` and `package` are never compiled
  in, so a script cannot reach them by any route. `dofile`/`loadfile` are
  removed from the base library too (there is no whitelisted script-library
  path yet).
- The bindings live in `libs/lua-bus` (`supple::lua`): `Script` runs source
  against a `cmd::Dispatcher`, `Runtime` keeps one named state per script
  (globals and subscriptions persist) and delivers the bus's events to the
  `q.on` handlers after the command that emitted them. `script.run` is on the
  bus in both the engine and the sandbox.
- `libs/command-bus` hosts the generators (`markdownReference`,
  `mcpToolList`, `luaStubs`); the LuaLS annotations are committed as
  `skill/generated/{engine,sandbox}.lua` behind a drift test.
- `q.batch` uses `cmd::Dispatcher::transact`, which wraps a run of commands in
  one host transaction — the same one-undo-step guarantee as the bus's `batch`.

## Consequences

Scripts are thin: all validation happens in the bus with the same actionable
errors agents see. Lua's error model maps cleanly onto `supple::Error`.

## Alternatives considered

- **Luau** (MIT): gradual static types and a built-in sandbox model are
  attractive for agent-written scripts, but it is a much larger C++ codebase,
  less familiar to most LLMs and users than plain Lua, and our typed bus
  already validates every call. Revisit if script type errors become a real
  support burden.
- **LuaJIT** (MIT): faster, but Lua 5.1 semantics and speed is irrelevant for
  bus-bound scripts.
- **Lua 5.5** (installed system-wide): newer and less known to LLMs/tooling;
  5.4 remains maintained.
- JavaScript (QuickJS, MIT): larger runtime, weaker embedding story here.
