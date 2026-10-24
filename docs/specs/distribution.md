# Distribution contract

ADR-0048 defines the layouts and rationale. Product identity comes exclusively
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
also resolves resources from its containing build tree. `engine exec` uses
the same stock discovery as the GUI and other engine modes.

Windows GUI startup creates no console; CLI invocations preserve redirected
output and attach to a parent console when available. Engine/MCP/bridge/
scanner remain console programs. macOS's bundle declares microphone access,
high DPI, the configured minimum OS, product identity and icon. The build
keeps a `daw/gui/<slug>` symlink for existing launch commands; CTest uses the
actual executable target path.

Artifacts are `<slug>-<version>-<platform>-<arch>.<ext>`: Linux x86_64 tar.xz,
macOS arm64 dmg/zip, Windows x86_64 exe/zip. Linux package audit rejects
unresolved/undeclared ELF dependencies and linked JACK. macOS finalization
rejects unbundled non-system absolute dylib references and verifies the
finished ad-hoc signature. Installed smoke checks exercise GUI snapshots,
stock plugin discovery without explicit paths, MCP's clean missing-app
failure, and required helper/resource presence in an isolated user profile.
