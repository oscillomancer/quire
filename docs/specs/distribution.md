# Distribution contract

ADR-0048 and ADR-0049 define the layouts and rationale. Product identity comes exclusively
from `cmake/Product.cmake`. `cmake --install` exposes components `Runtime`
(five programs, SDL3, resources, licenses, desktop integration sources) and
`Plugins` (fourteen stock CLAPs plus license/notices; no Script Player).
`package-runtime` builds exactly the needed distribution targets.

The executable-relative stock-plugin and skill lookup is public host API.
Linux: `../lib/<slug>/plugins` and `../share/<slug>/skill` from `bin/`.
Windows: `plugins/clap` and `skill` from the flat executable directory.
macOS: `../PlugIns/CLAP` and `../Resources/skill` from `Contents/MacOS/`.
Fonts are a sibling `fonts/` directory to the installed skill. Installed
paths take precedence over build-tree fallback; CMake's macOS GUI bundle
also resolves resources and bridge/scanner helpers from its containing build tree. `engine exec` uses
the same stock discovery as the GUI and other engine modes.

Windows GUI startup creates no console; CLI invocations preserve redirected
output and attach to a parent console when available. Engine/MCP/bridge/
scanner remain console programs. macOS's bundle declares microphone access,
high DPI, the configured minimum OS, product identity and icon. The build
keeps a `daw/gui/<slug>` symlink for existing launch commands; CTest uses the
actual executable target path.

Artifacts are `<slug>-<version>-<platform>-<arch>.<ext>`: Linux x86_64 tar.xz,
macOS arm64 dmg/zip, Windows x86_64 exe/zip. Linux packages use the `linux-package` preset's hidden static C++ runtimes
for all five programs and the stock CLAPs; the parent forwards configure
flags without modifying shared plugin-template files. Release CI uses Clang 22
with GCC 12 libstdc++ on Ubuntu 22.04. Package audit rejects
unresolved/undeclared ELF dependencies, shared libstdc++/libgcc_s, C++ runtime
exports and linked JACK. macOS finalization
rejects unbundled non-system absolute dylib references and verifies the
finished ad-hoc signature. Installed smoke checks exercise GUI snapshots,
stock plugin discovery without explicit paths, MCP's clean missing-app
failure, and required helper/resource presence in an isolated user profile.
macOS plugin notices stay inside each CLAP bundle's `Contents/Resources`;
the surrounding `PlugIns` directory contains code only. Finalization signs
nested binaries, then CLAP bundles, then the application and its main executable.
This follows Apple's [nested-code placement and signing rules](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/Procedures/Procedures.html).
The installed private SDL dylib contains the resolved library bytes even when
the package manager's imported path is a symlink. Bundle finalization rejects
dangling symlinks and links that leave the application.

Installers upgrade their own prior installation and refuse foreign target
collisions. Windows checks mapped binaries before uninstalling the previous
version, remembers the external-plugin choice, and removes only staged file
paths with nonrecursive directory cleanup.
Windows persists relative UTF-16 ownership inventories in `.installed-files`
and `.installed-plugins`. Upgrade preflight checks both the app and selected
external CLAP files before removal; unowned new paths are refused. `/PLUGINS`
selects the optional component for silent installation. Uninstall refuses
reparse points in private app/plugin directory chains before any deletion.
Linux similarly refuses symlinked private payload ancestors, keeping the
manifest on refusal; external bin/desktop/icon/CLAP entries remain path-based.
Linux records owned payload and
integration paths, preserves user files during upgrade/removal, and prunes
only owned directory ancestors. Linux upgrades remember external-plugin opt-in
and preflight all new destinations against previous ownership before uninstalling,
so a collision leaves the old version intact. Optional macOS CLAP installation documents
recursive quarantine removal after copying trusted plugins.

`SUPPLE_PACKAGE_ONLY=ON` (with tests off) builds the template's existing CLAP
and notice targets without unshipped wrappers. Normal native builds retain
all plugin formats and tests; no shared template file changes are required.
