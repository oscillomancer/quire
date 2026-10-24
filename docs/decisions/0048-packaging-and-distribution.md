# ADR-0048: Packaging and distribution

**Status:** Accepted, 2026-10-24. Amends ADR-0034's build-only CI scope.

**Runtime amendment, 2026-10-24:** [ADR-0049](0049-isolated-linux-package-runtimes.md)
replaces shared Linux C++ runtimes with hidden static runtimes for in-process
plugin compatibility. The original decision below is retained for history.

## Context

Users need runnable downloads without a compiler, and agents need a stable
MCP command outside the GUI. There are no paid signing accounts. Stock CLAP
plugins remain independently usable in other hosts, with MIT notices.

## Decision

All names and versions derive from `cmake/Product.cmake`. CMake install
components `Runtime` and `Plugins` stage five programs (GUI, engine, MCP proxy,
bridge, scanner), fourteen stock CLAPs, the agent skill, fonts and licenses.
The script player and development tools are excluded. Packaging builds only
`package-runtime`; it does not change the template plugin infrastructure.
The GUI uses installed fonts first and the checkout's fonts in a build tree.

Linux ships a relocatable tar.xz, runnable in place. `bin/` contains the
programs; `lib/<slug>/` contains SDL3 and private C++ runtimes, with stock
plugins in `lib/<slug>/plugins/`. Resources live in `share/<slug>/`, license
texts in `share/doc/<slug>/`, and desktop/icons in standard XDG subfolders.
Inherited `$ORIGIN/../lib/<slug>` DT_RPATH covers dlopened stock CLAPs too.
The template plugins dynamically link libstdc++; bundling private libstdc++
and libgcc avoids a newer runtime requirement without changing their shared
stack. Their actual copyright, GPL and runtime-exception texts ship together.
A dependency audit rejects missing or unexpected shared libraries and any
linked JACK; graphics, glibc, X11 and ALSA stay OS supplied. Build releases
in an Ubuntu 22.04 container, never on a newer workstation for publication.

The tarball's user-space installer defaults to `~/.local`, copies its tree to
`opt/<slug>`, links programs in `bin`, and copies desktop and hicolor icons.
`--prefix` selects another absolute prefix. Existing files cause an error.
`--plugins`, off by default, copies the stock CLAPs and notices to `~/.clap`.
The uninstaller records and removes only installer-owned paths. No JSON
file association is created, and no audio or agent configuration is changed.
Flatpak/Snap are unsuitable here: host plugin folders, bridge subprocesses,
real-time hosting and an external MCP agent crossing the sandbox complicate
operation. AppImage is deferred: random mount paths complicate a stable MCP
command and FUSE is an additional host requirement.

macOS ships an Apple Silicon .app in a dmg (with an Applications shortcut
and a separate optional CLAP folder), plus a zip. All five programs live in
`Contents/MacOS`, stock CLAPs in `Contents/PlugIns/CLAP`, resources in
`Contents/Resources`, SDL3 in `Contents/Frameworks`. Relative install names
are fixed before signing nested code and then the bundle ad hoc. The plist
sets the deployment minimum, high DPI support and microphone permission.
There is no notarization. Gatekeeper first-launch steps are documented;
macOS 15+ uses Privacy & Security → Open Anyway, or recursive quarantine
removal for the trusted downloaded app.

Windows ships a per-user NSIS installer and portable zip. Programs and
runtime DLLs are flat, with `plugins/clap`, `skill`, `fonts` and `licenses`.
The GUI uses the Windows subsystem and attaches to a parent console for
command-line output; helpers retain the console subsystem. NSIS uses only
its Zlib-licensed stub/compressor, avoiding Inno Setup/WiX and NSIS LZMA
license exposure. Installation needs no administrator, registers an HKCU
uninstaller and Start-menu shortcut, and defaults to LocalAppData/Programs.
An unchecked CLAP component uses the specification's per-user search root,
`%LOCALAPPDATA%\Programs\Common\CLAP`, under a product subfolder (hosts recurse).
Unsigned installers trigger SmartScreen. Recommend the installer because
portable zip extraction can propagate Mark of the Web to nested code.

The separate `<slug>-mcp` proxy ships beside the app/helpers; agents register
that stable executable and can reach the running app's control socket. The
engine also retains its independent `--mcp` mode. No client configuration is
silently installed.

`package.yml` runs only on manual dispatch, builds Release on each native
platform, smokes the installed artifact and uploads downloads. Its optional
release step creates or updates only a draft `v<version>` release; publication
is manual. Existing native build/test CI remains a separate required check.

## Consequences

We own a small installer and dependency audit rather than a system package
repository. Users manually remove unsigned-application restrictions. Native
CI verifies packaging changes on all three platforms; running Linux binaries
alone cannot validate Apple signing, Windows startup or installer behavior.
