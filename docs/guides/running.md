# Running Quire

Commands use the current binary names; they follow `PRODUCT_SLUG` in
`cmake/Product.cmake`.

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

The app scans, in this order: the stock plugins built beside it
(`build/release/plugins/clap`), then `CLAP_PATH`, `~/.clap`, the system CLAP
folders and the VST3 folders. The first copy of a plugin id wins, so an older
stock plugin installed in `~/.clap` is listed as shadowed and never loaded.
`--plugin-path P` (repeatable) replaces the whole list.

To add an effect or instrument: the **fx** button on a track (it opens the
plugin browser when the track has none, the chain otherwise), **+ FX** in the
mixer, or Track > FX: add plugin….

## Driving Quire from Claude Code (MCP)

`quire-mcp` serves the running app's commands as MCP tools over stdio: what an
agent does lands in the same undo history as your own edits. Register it once:

```sh
claude mcp add -s user quire -- ~/Projects/quire/build/release/mcp/quire-mcp
```

Start Quire first, then the Claude Code session (or `/mcp` to reconnect). If
Quire restarts while Claude Code runs, the next tool call reconnects by
itself. `--instance NAME` picks a second running app (`quire --instance NAME`).

The bundled skill teaches an agent the commands, units and notation. Link it
into Claude Code's skills:

```sh
ln -s ~/Projects/quire/skill ~/.claude/skills/quire
```

Without the GUI, `build/release/host/engine/quire-engine --mcp` is a headless
MCP server with the same commands (renders, no window).

## The agent panel

View > Agent panel talks to an Anthropic or an OpenAI-compatible endpoint
(Settings; the key is kept in `~/.config/quire/agent.json`, mode 0600).
Local servers work: for llama.cpp's `llama-server`, use base URL
`http://127.0.0.1:8000` (or `.../v1`) and start the server with `--jinja` so it
accepts tools. The tool list is about 17k tokens, so give the server a context
of 32k or more.
