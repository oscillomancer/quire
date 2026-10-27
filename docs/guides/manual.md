# Quire manual

This manual covers using Quire once it is installed ([Installing
Quire](installing.md)) or built ([Building Quire](building.md)): starting
the app, audio devices, projects, plugins, and connecting an AI agent.

The workflow follows REAPER's, so most of the arrange view, mixer, FX chains,
routing and actions list will feel familiar from it. Every command the app
runs is also listed in the actions list (`Shift+/` or `Ctrl+Shift+P`).

## Starting Quire

Start Quire from the application menu, the Start menu or Finder, or from a
terminal:

```sh
quire                 # a new project
quire song.json       # open one
quire --help          # every command-line option
```

Command names follow the product name. In a build tree, the app is
`build/<preset>/daw/gui/quire`.

## Audio

Quire plays through JACK on Linux (PipeWire's JACK support works) and
through the system's audio device on macOS (Core Audio) and Windows (WASAPI
by default, or ASIO).

**Edit > Audio device…** (also a row in **Edit > Preferences…**) chooses the
audio system, the output and input devices, the sample rate and the buffer
size. Quire remembers the choice and starts with it next time. Each change
restarts the running engine at once.

- **Linux:** the rate and buffer size are requests to PipeWire, which shares
  the graph with other apps. It changes the rate only while no other client
  is playing, and only to a rate it allows. Choose **Force** to hold both on
  the whole graph while Quire's engine runs. Other apps then run at Quire's
  rate and buffer size too, and the device can keep them after Quire quits.
- **macOS:** a chosen rate switches the device's own rate, which it keeps
  after Quire quits. The buffer size is
  the device's I/O size. Allow microphone access when macOS asks, so that
  recording can use your input.
- **Windows:** WASAPI shares the device with other apps and converts other
  rates. ASIO uses one device for input and output, and its driver may round
  the buffer size.

If the device is missing or busy when Quire starts, the app still opens. The
Audio device dialog then opens and says why. Choose another device, or
**Restart audio** once the device is ready. The dialog's status lines show
what is running: the backend, rate, buffer size, device period and output
latency.

## Projects

A project is a JSON file. Its media lives in the `media/` folder beside it.

Until a project is saved, its recordings go to a folder of its own under the
data folder (below), in `unsaved/<start time>-<pid>/media/rec`. The first
save moves them into `media/` beside the project file. Quitting, or starting
another project, without saving deletes that folder after the app has asked.
A crashed run leaves its folder behind, recordings included.

Renders go to `~/Documents/quire-media` unless you choose another folder in
the render dialog. The folder you choose is remembered.

### Where Quire keeps its files

| | Settings (preferences, menus, controls, appearance) | Data (unsaved projects, DSP scripts) |
| --- | --- | --- |
| Linux | `~/.config/quire` (`$XDG_CONFIG_HOME`) | `~/.local/share/quire` (`$XDG_DATA_HOME`) |
| macOS | `~/Library/Application Support/quire` | `~/Library/Application Support/quire` |
| Windows | `%APPDATA%\quire` | `%APPDATA%\quire` |

Uninstalling Quire leaves these folders and your projects in place.

## Plugins

Quire hosts CLAP and VST3 plugins. It looks for them in this order:

1. its own stock plugins, installed privately with the app (in a build tree,
   `build/<preset>/plugins/clap`);
2. `CLAP_PATH`, then the user and system CLAP folders (`~/.clap` on Linux,
   `~/Library/Audio/Plug-Ins/CLAP` on macOS, `%LOCALAPPDATA%\Programs\Common\CLAP`
   and `%COMMONPROGRAMFILES%\CLAP` on Windows);
3. the VST3 folders.

The first copy of a plugin id wins. An older copy of a stock plugin installed
for other hosts is listed as shadowed and never loaded. `--plugin-path P`
(repeatable) replaces the whole list.

To add an effect or instrument, use the **fx** button on a track (it opens
the plugin browser when the track has no effects, the chain otherwise),
**+ FX** in the mixer, or **Track > FX: add plugin…**.

## Agents (MCP)

Quire is built to be driven by an AI agent as well as by hand. `quire-mcp`
serves the running app's commands as MCP tools over stdio. What an agent
does lands in the same undo history as your own edits. The bundled skill
teaches the agent Quire's commands, units and notation.

### Connecting from the app

**Help > Connect an agent…** registers this copy's MCP server with Claude
Code, Codex or Hermes:

1. Choose the agent and review the command shown.
2. Click **Connect**, or **Copy command** to run it yourself. Copied commands
   are for PowerShell on Windows.
3. Restart or reconnect the agent. Start Quire before you use the agent.

The result shows the command's exit status and output. If a server of that
name is already registered, check its path in the agent before replacing it.

Connect also links the bundled skill into the agent's user skills folder, as
`quire/`:

| Agent | Skills folder |
| --- | --- |
| Claude Code | `~/.claude/skills` (or `$CLAUDE_CONFIG_DIR/skills`) |
| Codex | `~/.agents/skills` |
| Hermes | `~/.hermes/skills` (or `$HERMES_HOME/skills`; `%LOCALAPPDATA%\hermes\skills` on Windows) |

The dialog shows the path before you click. If a skill of that name is
already there, or in Codex's older `~/.codex/skills`, Connect leaves it as
it is and says so.

**Help > MCP server…** shows this copy's absolute paths, a headless command
and stdio/JSON instructions for any MCP client. **Help > Download skill…**
copies the bundled skill into a folder you choose, under `quire/`.
**Download skill link…** creates a symlink (a directory junction on Windows)
instead, which follows updates to the installation; keep the installation in
place when you use one. Use these for other agents, or to replace a skill
that Connect left in place. Replacing an existing skill or link asks first,
and unrelated files and folders are never touched.

On macOS, move Quire.app into Applications before connecting an agent. If
macOS runs it from a temporary App Translocation location, Help shows how
to restart it from a stable location instead of these controls. See
[Installing Quire](installing.md#macos-apple-silicon-macos-144) for
clearing quarantine.

### Connecting by hand

The installed MCP adapter and skill are here:

| Platform | `quire-mcp` | Skill |
| --- | --- | --- |
| Linux | `~/.local/bin/quire-mcp` (installed) or `bin/quire-mcp` in the extracted tree | `share/quire/skill` in the tree |
| macOS | `/Applications/Quire.app/Contents/MacOS/quire-mcp` | `Quire.app/Contents/Resources/skill` |
| Windows | `%LOCALAPPDATA%\Programs\Quire\quire-mcp.exe` | `skill` in the installation folder |
| Build tree | `build/<preset>/mcp/quire-mcp` | `skill/` in the repository |

Register the adapter by its absolute path, and link the skill into the
agent's skills folder. For a build tree, from the repository root:

```sh
claude mcp add -s user quire -- "$PWD/build/release/mcp/quire-mcp"
ln -s "$PWD/skill" ~/.claude/skills/quire

codex mcp add quire -- "$PWD/build/release/mcp/quire-mcp"
hermes mcp add quire --command "$PWD/build/release/mcp/quire-mcp"
```

Start Quire first, then the agent (in Claude Code, `/mcp` reconnects). If
Quire restarts while the agent runs, the next tool call reconnects by itself.
To drive a second running copy, start it with `quire --instance NAME` and
pass `--instance NAME` to `quire-mcp`.

Without the GUI, `quire-engine --mcp` is a headless MCP server with the same
commands: it renders, but has no window.
`quire-engine play FILE --seconds 10` plays a saved project for ten seconds;
add `--backend dummy` to run without an audio device. Numeric options must be
complete numbers, and `--seconds` must be finite and nonnegative. Invalid values
report the option and how to correct it before playback starts.

### Other clients and local models

Any MCP client can use the same server. Give agents without skill support
`skill/SKILL.md` as instructions; it lists the guides to read when needed.
The tool list is large (about 130 tools). If you run a local model behind a
harness that sends every tool with each request, give the model a context of
32k tokens or more. llama.cpp's `llama-server` needs `--jinja` to accept
tools.
