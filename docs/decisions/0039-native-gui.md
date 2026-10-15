# ADR-0039: Native DAW windowing and agent panel

**Status:** Accepted, 2026-10-14. Amends ADR-0005's Linux-only windowing
boundary and ADR-0035's remaining GUI gate. Editor embedding is a separate port.

## Decision

Use SDL3's X11 driver on Linux, Cocoa on macOS, and Windows on Windows.
Keep one OpenGL core context per window: 3.3 on Linux/Windows, 4.1 forward
compatible on macOS. NanoVG and the existing bulk renderers are unchanged.
A small original Windows loader resolves the GL entry points through SDL;
no loader dependency or Metal renderer is added. SDL's imported headers are
marked system headers for the DAW warning policy. Windows build glue supplies
four standard post-1.1 GL enums required by the older stock GUI copies; their
template/shared-stack sources remain unchanged. Fonts are read through the
portable file helper, including wide Windows paths.

Design units remain the toolkit's coordinate system. Cocoa's Retina density
maps them to framebuffer pixels. On Windows, SDL's per-monitor display scale
also maps window/input coordinates to design units; pointer, drop and IME
caret coordinates use the same factor. Snapshots capture actual framebuffer
pixels, so width/height equal design dimensions times the pixel ratio.

The shared GUI service watcher is extracted from plugin_windows into
service_watcher. It borrows Waitables and parks through waitFor, interrupted
by a WakeEvent on registration changes or shutdown. Ready registrations stay
out of the wait set until dispatched; registration serials and generation
checks discard stale snapshots after descriptor reuse. App/model notifications
use serviceWaitable()/ModelLoop::waitable(). CLAP POSIX descriptors are wrapped
as Waitables. Permanent wait failure still ends the loop, without a safety
poll. The old Linux test injection API is retained in a Linux-only adapter.

The logical primary modifier (the toolkit's kCtrl bit) is Command on macOS,
Control on Linux/Windows. All spec Ctrl shortcuts and pointer gestures follow
that rule. macOS displays Cmd in shortcut labels, accepts both Ctrl and Cmd
in keymap documents, and uses the secondary kSuper bit for physical Control.
Shift and Option/Alt retain their meanings. Cmd-Q invokes ui.quit; SDL's
application-menu Quit event uses the normal shutdown/flush path. No custom
native menu hierarchy is added. Interactive native audio defaults to dummy,
as in the engine; --audio portaudio opts into device audio. Finite/offscreen
runs retain their default of no backend.

The agent HTTP client keeps libcurl and the existing streaming/cancellation
contract. Cooperative jobs use core-base's Apple-compatible stop layer. Peak/writer
condition-variable predicates include stop state, with stop requests protected
by the same mutex to prevent a lost shutdown wake. GL-free sidecar tests also
compile against the compatibility types on Linux and run under TSan.
Tests use Winsock on Windows and POSIX sockets elsewhere, suppress SIGPIPE,
and handle partial sends. Agent config uses the platform configuration root.
POSIX publication creates a 0600 exclusive temporary, writes/fsyncs, renames
and syncs the directory. Windows creates the temporary with a protected DACL
containing one allow ACE for the current token user, before writing secrets;
FlushFileBuffers and same-directory MoveFileExW replace it. ACL-less file
systems fail rather than exposing a key. This is no permissions relaxation.
Existing external readers denying delete sharing can prevent replacement;
that failure preserves the previous config and removes the temporary file.

## Validation and boundaries

Linux uses SDL's EGL offscreen driver and retains its existing golden PNGs.
macOS/Windows --offscreen uses hidden native windows in a desktop session,
without requiring SDL's EGL offscreen backend. Rendering fixtures require a
working native GL context; native snapshots check dimensions/content rather
than Linux pixel identity. Native platform-smoke.png is retained as a CI artifact.
Logic, HTTP/config, peak-worker and watcher tests run on all platforms.
Real plugin-display tests remain opt-in only when SUPPLE_GUI_DISPLAY_TEST is
exactly 1; native embedding belongs to the editors worker.

Windows hosted runners use a CI-only, SHA-256-pinned Mesa 24.3.4 MSVC driver
from pal1000/mesa-dist-win. Only its OpenGL/dispatch DLLs are copied beside
GUI test executables, and llvmpipe is selected. There is no system driver
registration, product dependency or workstation installation. Mesa's license
is MIT; LLVM components use Apache-2.0 with LLVM exception. macOS runners use
their WindowServer session. POSIX GUI socket/screenset Python integration
scripts remain Linux-only until separately ported. macOS cold-start timing
currently reports -1 (unavailable); CPU/frame/idle measurements still run.
Native editor embedding and real-device/monitor trials are separate validation.

References: [SDL high DPI](https://wiki.libsdl.org/SDL3/README-highdpi),
[Mesa Windows builds](https://github.com/pal1000/mesa-dist-win/releases/tag/24.3.4),
[Mesa license](https://docs.mesa3d.org/license.html).

**2026-10-14 validation note:** Windows CI also deploys the same pinned DLLs
beside stock plugin test/probe executables, matching the template's Windows CI.
The player's Win32 embedding tests require that software GL setup too.

**2026-10-14 bridge integration note:** After ADR-0037 landed on `port`, the
DAW executable and GUI tests depend unconditionally on the bridge executable
on all platforms. Native editor embedding remains the separate editor port.

**2026-10-14 stock renderer integration note:** Monorepo CMake selects the
stock projects' existing OpenGL renderer on macOS (`<PREFIX>_METAL=OFF` and
`SUPPLE_METAL=OFF`). Their standalone default Metal renderer requires a
NanoVG Metal dependency that this repository does not vendor. No template
files or pins change, and no new dependency is introduced.

**2026-10-14 repaint validation note:** MIDI rectangle borders use framebuffer
pixel coordinates rather than interpolated local coordinates. Apple's software
renderer rounded an interpolated one-pixel border threshold differently under
partial and full scissors. The same-platform incremental/full pixel comparison
remains exact; native cross-platform golden comparisons remain disabled.

**2026-10-14 editor integration note:** ADR-0040's native editor implementation
is now combined with the GUI shell. Its NativeWindow accessor and native sizing
are preserved alongside the extracted shared Waitable watcher. GUI editor
integration tests build natively; real plugin display tests retain their exact
SUPPLE_GUI_DISPLAY_TEST=1 opt-in. Linux pipe/poll fixtures stay Linux-only.
