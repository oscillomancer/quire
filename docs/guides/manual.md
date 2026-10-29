# Quire manual

This manual covers using Quire once it is installed ([Installing
Quire](installing.md)) or built ([Building Quire](building.md)): starting
the app, audio devices, projects, plugins, and connecting an AI agent.

The workflow follows REAPER's, so most of the arrange view, mixer, FX chains,
routing and actions list will feel familiar from it. The actions list includes
the public commands and GUI actions (`Shift+/` or `Ctrl+Shift+P`,
`Cmd+Shift+P` on macOS).

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

When monitoring an armed input, the latency comes from the device, input FX,
and effects on that input's route to the master. Effects on unrelated playback
tracks do not delay it. If the input feeds both a direct path and a bus with
latency, the shorter path is delayed so the two stay aligned.

## Projects

A project is a JSON file. Its media lives in the `media/` folder beside it.
Project and FX chain files accept oversampling factors 1, 2, 4 or 8. If a
hand-edited file uses another factor, loading reports the field and allowed values.

Until a project is saved, its recordings go to a folder of its own under the
data folder (below), in `unsaved/<start time>-<pid>/media/rec`. The first
save moves them into `media/` beside the project file. Quitting, or starting
another project, without saving deletes that folder after the app has asked.
A crashed run leaves its folder behind, recordings included.

New, Open and Quit (including File > Quit, Cmd+Q on macOS and closing the main
window) first stop an active recording and add its takes to the
project, then ask whether to save the changes. Cancel keeps the recorded takes;
Undo removes that recording. If stopping fails, the error is shown and the
requested action does not continue. If an agent starts another recording or
edits the current project while that question or its Save As dialog is open,
the app stops the new recording and asks again before saving or leaving.
A changed branch also requires a new decision.

If another project replaces the current one while the save-changes question is
open, any button cancels the old request with a notice, leaving the replacement
project untouched. File and folder dialog results are also ignored with a notice
if the project was replaced; open the dialog again. If edits arrive
while Open is waiting, Quire asks about unsaved changes again before opening.

Renders go to `~/Documents/quire-media` unless you choose another folder in
the render dialog. The folder you choose is remembered.

### Where Quire keeps its files

| | Settings (preferences, menus, controls, appearance) | Data (unsaved projects, DSP scripts) |
| --- | --- | --- |
| Linux | `~/.config/quire` (`$XDG_CONFIG_HOME`) | `~/.local/share/quire` (`$XDG_DATA_HOME`) |
| macOS | `~/Library/Application Support/quire` | `~/Library/Application Support/quire` |
| Windows | `%APPDATA%\quire` | `%APPDATA%\quire` |

Uninstalling Quire leaves these folders and your projects in place.

## Automation

To edit an automation item's points, its amplitude magnitude must be greater than
0.000000001. If it is zero or too small to invert, the app asks you to restore
the amplitude first; the source curve is preserved.

If another edit changes an automation item's amplitude, baseline or geometry
while you drag its points, header or edge, the drag is cancelled with a message.
Start the drag again using the updated item.

## Plugins

Quire hosts CLAP and VST3 plugins. It looks for them in this order:

1. its own stock plugins, installed privately with the app (in a build tree,
   `build/<preset>/plugins/clap`);
2. `CLAP_PATH`, then the standard CLAP folders: user before system on Linux
   (`~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap`) and macOS
   (`~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP`);
   system before user on Windows (`%COMMONPROGRAMFILES%\CLAP`,
   `%LOCALAPPDATA%\Programs\Common\CLAP`);
3. `VST3_PATH`, then the standard VST3 folders in the same platform order.

The first copy of a plugin id wins. An older copy of a stock plugin installed
for other hosts is listed as shadowed and never loaded. `--plugin-path P`
(repeatable) replaces the whole list.

To add an effect or instrument, use the **+** beside **fx** on a track or
mixer strip, or **Track > FX: add plugin…**. The **fx** button opens the
track's FX chain, including when it is empty. In the mixer, right-click a
plugin to rename, duplicate or delete it; a duplicate keeps the plugin's
current settings and goes right after it. Alt-click (Option-click on macOS)
also deletes a plugin.
In the chain window, Ctrl-click (Cmd-click on macOS) toggles a row in the
selection, including the active row. Effects moved to another track leave
that window’s selection.

## Agents (MCP)

Quire is built to be driven by an AI agent as well as by hand. `quire-mcp`
serves the running app's commands as MCP tools over stdio. What an agent
does lands in the same undo history as your own edits. The bundled skill
teaches the agent Quire's commands, units and notation.

Saved `dsp.test` audio and `dsp.preview` pictures use readable, shortened
filenames with a hash when a plugin ID needs escaping or truncation, or ends
in the reserved `-` plus 16 hexadecimal digits suffix. Stock plugin filenames
stay the same. Use the returned `path` to open them; the `plugin` field retains
the full plugin ID.

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
already there, or in Codex's older `~/.codex/skills` (or `$CODEX_HOME/skills`),
Connect leaves it as it is and says so.

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
agent's skills folder. For a build tree, from the repository root in a
POSIX shell (Linux or macOS):

```sh
claude mcp add -s user quire -- "$PWD/build/release/mcp/quire-mcp"
mkdir -p ~/.claude/skills
ln -s "$PWD/skill" ~/.claude/skills/quire

codex mcp add quire -- "$PWD/build/release/mcp/quire-mcp"
hermes mcp add quire --command "$PWD/build/release/mcp/quire-mcp"
```

Start Quire first, then the agent (in Claude Code, `/mcp` reconnects). If
Quire restarts while the agent runs, the next tool call reconnects by itself.
To drive a second running copy, start it with `quire --instance NAME` and
pass `--instance NAME` to `quire-mcp`.

Replace `release` with your build preset (`macos` on macOS). On Windows,
use the app's **Copy command** button for a PowerShell command with this
installation's paths.

Without the GUI, `quire-engine --mcp` serves the engine's project and audio
commands and can render offline. GUI-only state and actions require the app.
`quire-engine play FILE --seconds 10` plays a saved project for ten seconds;
add `--backend dummy` to run without an audio device. Numeric options must be
complete numbers, and `--seconds` must be finite and nonnegative. Invalid values
report the option and how to correct it before playback starts.

For headless recording, stop the transport and save the project before closing
an MCP connection or stopping the socket server. Shutdown finishes capture but
does not save the project. Unsaved audio files can be recovered; unsaved MIDI
recordings and other session edits are lost when the process exits.

Output recording is not supported yet. If a loaded project has an armed track
in output record mode, recording fails before creating any files. Switch that
track to input mode with `track.set {track: "tr_1", record_mode: "input"}`.

### Other clients and local models

Any MCP client can use the same server. Give agents without skill support
`skill/SKILL.md` as instructions; it lists the guides to read when needed.
The tool list is large (over 140 tools). If you run a local model behind a
harness that sends every tool with each request, give the model a context of
32k tokens or more. llama.cpp's `llama-server` needs `--jinja` to accept
tools.

### Freezing a channel

Choose **Freeze** from a track header or mixer strip's context menu to record its
items, take effects and complete insert chain to project media. The job reports
progress through the existing job system (`job.get`, `job.cancel`). When it
finishes the channel shows **[Frozen]**, its items are dimmed and marked, and its
plugins are released. Fader, pan, mute, solo, their automation, sends and parent
routing remain available. To change the sound inside the recording, choose
**Unfreeze** from the same menu. Each completed operation is one undo step.

Agents use `track.freeze {"track":"tr_1","job":true}` and
`track.unfreeze {"track":"tr_1"}`. `track.list` reports frozen state and cache
metadata; `session.describe` marks frozen channels. Work on other tracks can
continue during a freeze. Completion refuses if the source sound, input files,
project/branch or latency schedule changed; submit the freeze again.

Disarm the channel and remove external audio, sidechain/key or MIDI inputs first.
V1 requires unity master playrate without an active rate envelope. Tempo, meter,
project sample rate and master-rate edits name the frozen tracks that must be
unfrozen first. Missing plugins or unavailable media must be repaired before
unfreezing. Infinite or unsettled tails refuse capture; finite reported tails
and a one-second quiet window are retained, up to ten minutes of tail. A bridge
that discovers a longer tail after rendering asks for in-process isolation.

Freeze keeps a particular offline performance. A random or transport-sensitive
plugin may sound different on a fresh pass, seek or loop; internal live DSP
memory cannot be restored by a saved patch. Keep plugin binaries and plugin-owned sample files stable
during capture. Audio after an unreported delay longer than the quiet window
cannot be predicted. Playback at a different device rate resamples the cache.

Frozen files are kept like recorded media, including after unfreeze, because
undo, branches, autosaves and saved projects can still refer to them. Save and
Save As carry them with the project. Do not manually edit or remove those files
while any saved project or history still needs them.
