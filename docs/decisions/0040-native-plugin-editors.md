# ADR-0040: Native plugin editors

**Status:** Accepted, 2026-10-14. Amends ADR-0005, ADR-0013, ADR-0036 and
ADR-0037. Complements ADR-0039 (native DAW GUI, on port-gui).
Contracts: gui.md §4.5 and sandbox.md §1.

**2026-10-14 amendment:** [ADR-0041](0041-cocoa-editor-ordering.md) replaces
floating level/cooperative activation with normal-level relative ordering,
adds the owner window-number handle and DAW activation hook, and negotiates
POSIX 17 / Windows 18. The original decision below is retained as written.

**2026-10-15 amendment:** Live display-scale changes are now renegotiated
(`Instance::setEditorScale`, bridge `EditorScale`, POSIX 18 / Windows 19); the
"not renegotiated yet" limit below no longer applies. See sandbox.md §1.

**2026-10-15 amendment (placement):** The Cocoa bridge reports its window's
content frame (top-left origin, points) over the editor channel, `EditorOpen`
takes one to restore, and the DAW persists it as it does other editor
placements (POSIX 19 / Windows 20). The "placement ... not represented" limit
below is lifted for placement; focus tracking remains a limit. Backing-scale
changes of a bridge-owned window need no host action: sizes are points and the
view observes its own backing scale.

## Decision

- A borrowed `NativeWindow {kind, uintptr_t handle}` distinguishes X11 XIDs,
  Cocoa NSView pointers and Win32 HWNDs. `Window::nativeParent()` returns the
  SDL window's parent view/handle. The old X11 accessor and host overload remain
  compatible. Offscreen Linux windows have no native parent.
- Local CLAP editors select x11/cocoa/win32, embedded, and VST3 selects
  X11EmbedWindowID/NSView/HWND. Cocoa uses points and native backing scale;
  set_scale/content-scale requests apply only to Windows/Linux. GUI containers
  convert pixel editor sizes to design units using Window::pixelRatio; Cocoa
  editor sizes already use points. Pixel container sizing converts directly
  to SDL window coordinates without integer design-unit rounding, preserving
  fixed editor dimensions at fractional Windows scale factors.
  Resize requests raised during attached()
  must survive the open call. Editors release their view before their parent.
- Windows bridge editors use the parent's HWND across processes. The bridge
  initializes Per Monitor v2 DPI awareness before loading modules, matching
  SDL's GUI default. Its main loop waits for IPC and native messages together
  using MsgWaitForMultipleObjectsEx and dispatches bounded message batches.
  Mouse focus and input go directly to the native child. The host does not
  attach input queues globally or forward arbitrary keyboard messages.
- Cocoa pointers never cross processes as parents. The bridge creates an
  NSWindow and embeds the same CLAP/VST3 NSView editor there; no plugin floating
  extension is required. It owns sizing, show/hide, close and native pumping.
  The DAW retains an editor entry without an SDL placeholder window. Reopening
  a hidden editor raises the same NSWindow. Closing/destroying an instance
  releases the view before the NSWindow; bridge death closes all its windows.
  Activation uses macOS14's cooperative NSApplication activate API.
- POSIX protocol 15 / Windows 16 add one u8 parent kind before EditorOpen's
  u64 handle and f64 scale. Numeric kinds are none=0, x11=1, cocoa=2, win32=3,
  floating=4. Cocoa bridges require floating with handle zero, and create the
  actual parent themselves. Invalid/wrong-platform parents and nonpositive or
  nonfinite scales report host.editor.parent with /editor/parent and a hint.
  Old versions fail at mandatory subscription; no audio layout changes.
- CLAP timers stay main-thread deadlines on every platform. POSIX FD support
  remains advertised on Linux/macOS (both service descriptors through typed
  waits); it is absent on Windows. The macOS bridge uses CFFileDescriptor
  sources to wake AppKit's native event retrieval alongside IPC, without a
  periodic editor poll. AppKit is initialized lazily only for a floating editor.
  Each bridge loop iteration has an autorelease pool. SDL supplies the native
  loop for local DAW editors. The shared GUI watcher remains owned by port-gui.

## Validation and limits

Hermetic tests cover native CLAP/VST3 APIs, pointer width, partial-open cleanup,
resize negotiation, repeated open/close and malformed/old editor payloads.
Stock capability checks require a GUI build with staged stock modules;
fixture-only host builds skip that one check. Real native windows are opt-in
only (`SUPPLE_GUI_DISPLAY_TEST=1`); CI does not opt in. A headless build or
passing fixture test does not establish visual rendering, focus or IME behavior.

Cocoa bridge windows are separate application windows at floating level. They
cannot be transient children of an NSWindow in another process. Their desktop
placement and input focus are not represented in the DAW UI sidecar/MIDI-learn
focus predicate. The level can also put them above unrelated applications.
Windows cross-process keyboard accelerators, tab traversal and IME depend on
the plugin/native window manager; DAW shortcuts are not forwarded from children.
Live monitor/DPI changes are not renegotiated into a plugin's set_scale yet;
close/reopen adopts the new scale. Native DAW-side integration requires the
separate port-gui branch; this branch does not lift its CMake gate by itself.

API references: [AppKit event retrieval](https://developer.apple.com/documentation/appkit/nsapplication/nextevent(matching:until:inmode:dequeue:)),
[CFFileDescriptor](https://developer.apple.com/documentation/corefoundation/cffiledescriptor),
[Windows UI waits](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex),
[DPI initialization](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiawarenesscontext),
[cross-process parent DPI](https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows).
