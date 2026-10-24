# Running Quire

Commands use the current binary names; they follow `PRODUCT_SLUG` in
`cmake/Product.cmake`.

For downloaded packages, start with [Installing Quire](installing.md). The
installed GUI and helpers work outside the checkout; use the installed
`quire-mcp` absolute path when registering an agent. On macOS the build-tree
`daw/gui/quire` command links to the executable inside its app bundle.

## Build and start

```sh
cmake --preset release
cmake --build --preset release -j6
build/release/daw/gui/quire                 # a new project
build/release/daw/gui/quire song.json       # open one
```

Audio goes through JACK (PipeWire's JACK on this machine). If the device is
not ready, the app still opens; run `engine.start` from the actions list
(Shift+/) once it is.

## Unsaved projects

Until a project is saved, its recordings go to its own folder,
`~/.local/share/quire/unsaved/<start time>-<pid>/media/rec`. The first save moves
them into `media/` beside the project file. Quitting (or starting another
project) without saving deletes that folder, after the app has asked. A crashed
run leaves its folder behind, recordings included.

## Plugins

The app scans, in this order: its private installed stock plugins (or
`build/release/plugins/clap` in a build tree), then `CLAP_PATH`, `~/.clap`, the system CLAP
folders and the VST3 folders. The first copy of a plugin id wins, so an older
stock plugin installed in `~/.clap` is listed as shadowed and never loaded.
`--plugin-path P` (repeatable) replaces the whole list.

To add an effect or instrument: the **fx** button on a track (it opens the
plugin browser when the track has none, the chain otherwise), **+ FX** in the
mixer, or Track > FX: add plugin….

## Driving Quire from an agent (MCP)

`quire-mcp` serves the running app's commands as MCP tools over stdio: what an
agent does lands in the same undo history as your own edits. Register it once:

```sh
claude mcp add -s user quire -- "$PWD/build/release/mcp/quire-mcp"   # from the repository root
```

Start Quire first, then the Claude Code session (or `/mcp` to reconnect). If
Quire restarts while Claude Code runs, the next tool call reconnects by
itself. `--instance NAME` picks a second running app (`quire --instance NAME`).

The bundled skill teaches an agent the commands, units and notation. Link it
into Claude Code's skills:

```sh
ln -s "$PWD/skill" ~/.claude/skills/quire
```

Without the GUI, `build/release/host/engine/quire-engine --mcp` is a headless
MCP server with the same commands (renders, no window).

## Other MCP clients

Any MCP client can use the same server. For example:

```sh
codex mcp add quire -- "$PWD/build/release/mcp/quire-mcp"
hermes mcp add quire --command "$PWD/build/release/mcp/quire-mcp"
```

Give harnesses without skill support `skill/SKILL.md` as instructions; it
lists the guides to read on demand. The tool list is large (about 130 tools),
so with a local model behind a harness that sends every tool with each
request, give the model a context of 32k or more. llama.cpp's `llama-server`
needs `--jinja` to accept tools.
