# Installing Quire

Choose your platform's download from the release assets. Quire is unsigned;
only proceed with a download you trust. No paid signing accounts are used.
These packages install no audio service, driver or agent configuration.
Projects remain ordinary JSON files, without a system file association.

## Linux (x86_64)

Extract `quire-<version>-linux-x86_64.tar.xz`. You can run `bin/quire` directly
from the extracted tree, or install for your user:

```sh
./install.sh                         # ~/.local/opt/quire, links in ~/.local/bin
./install.sh --prefix /absolute/path # another user-writable prefix
```

Ensure `~/.local/bin` is on PATH for terminal launches. The application menu
entry works independently of PATH. Add `--plugins` only if other DAWs should
also find the stock CLAPs in `~/.clap`; Quire always has its own private copy.
Re-running the installer upgrades its own previous installation automatically;
pass `--plugins` again to keep external CLAP copies. Foreign paths are refused,
and unrelated user files and icon folders survive uninstall. X11/XWayland and OpenGL
are required; audio uses your existing JACK/PipeWire setup. Releases target
Ubuntu 22.04's glibc or newer. No root access or system changes are needed.

```sh
~/.local/opt/quire/uninstall.sh
# For a custom prefix, pass the same --prefix /absolute/path.
```

Uninstall leaves projects and user preferences intact and removes only
installed integration files and optional CLAP copies.

## macOS (Apple Silicon, macOS 14.4+)

Open the dmg and drag **Quire.app** to **Applications**. On first launch,
Gatekeeper may block this unsigned, ad-hoc-signed app. On macOS 15 and later,
use **System Settings → Privacy & Security → Open Anyway** after trying to
launch it (the old right-click → Open bypass is no longer sufficient).
Alternatively, for the trusted app you just installed:

```sh
xattr -dr com.apple.quarantine /Applications/Quire.app
```

Recursive removal also covers the bundled helpers and stock plugins. Allow
microphone access when macOS asks so recording can use your input device.
The dmg's **Stock plugins for other hosts** folder is optional: copy its
CLAP bundles and accompanying notices into
`~/Library/Audio/Plug-Ins/CLAP` (create the folder if needed). The app already
contains its own copy. Downloaded CLAP bundles can retain quarantine and be
blocked by other hosts. After copying trusted bundles, clear it recursively:

```sh
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/CLAP/<file>.clap"
```

Repeat for each bundle (or clear the folder for trusted plugins). The zip is an alternative to the dmg; place the
extracted app in Applications and follow the same first-launch steps.

To uninstall, quit the app and remove `/Applications/Quire.app`; remove any
optional CLAP copies you installed separately. Projects/preferences survive.

## Windows (x64, Windows 10 1903+)

Run `quire-<version>-windows-x86_64.exe`. If SmartScreen blocks the unsigned
installer, choose **More info → Run anyway** for a trusted download. It
installs only for you under `%LOCALAPPDATA%\Programs\Quire`, without admin
rights, and adds a Start-menu shortcut and an Installed Apps entry.

Leave **Stock plugins for other hosts (CLAP)** unchecked unless you want
other hosts to see them. That component installs under
`%LOCALAPPDATA%\Programs\Common\CLAP\quire`, the standard per-user CLAP search
root (hosts scan subfolders). The app has its own private plugins regardless.
Re-running the installer upgrades the registered installation and remembers
the optional plugin choice. Close the app and other hosts using its plugins
first; locked files cause a retry message before replacement. A new target
folder must be empty. Uninstall through Installed Apps or the installation's
`uninstall.exe`; only installer-owned files are removed, preserving projects
and other unrelated files.

The portable zip also runs from its extracted folder. The installer is the
recommended download: Windows can propagate Mark of the Web from a zip to
its nested binaries, causing additional helper/plugin launch restrictions.
The installer and uninstaller support `/S`; `/D=C:\absolute\path` must be
the last installer argument. Optional plugins stay off in a silent install.

## Connecting an agent

Register the installed `quire-mcp` executable using an absolute path. For
example, on Linux:

```sh
claude mcp add -s user quire -- "$HOME/.local/bin/quire-mcp"
```

On macOS the helper is `/Applications/Quire.app/Contents/MacOS/quire-mcp`;
on Windows it is `%LOCALAPPDATA%\Programs\Quire\quire-mcp.exe`. Start the
app first. The bundled skill is `share/quire/skill` under the Linux tree,
`Contents/Resources/skill` in the macOS app, or `skill` on Windows. Link it
into your agent's skills folder, or give it `SKILL.md` as instructions.
[Running Quire](running.md) covers MCP clients and headless use.
