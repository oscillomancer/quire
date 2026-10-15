# ADR-0041: Cocoa bridged editor ordering

**Status:** Accepted, 2026-10-14. Amends ADR-0040.
Contracts: core-base.md, gui.md §4.5 and sandbox.md §1.

## Context

An accessory bridge's NSFloatingWindowLevel editor stays above unrelated
applications after the user switches away from the DAW. Opening the editor
also activates the bridge, taking activation away from the DAW unnecessarily.
A cross-process NSWindow cannot be a transient child of the DAW window.

## Decision

Use NSNormalWindowLevel and `orderWindow:NSWindowAbove relativeTo:` with the
DAW owner's server `NSWindow.windowNumber`. The Floating NativeWindow handle
carries that unsigned 32-bit number, or zero for standalone clients without
an owner. Never send an NSView or NSWindow pointer across processes. Embedded
Cocoa inputs still become Floating/zero; explicit Floating inputs retain the
number. Reject oversize numbers before window creation. POSIX protocol 17 /
Windows 18 prevent mixing this semantic contract with zero-only v15/v16 peers;
the payload layout, operation numbers and audio arena are unchanged.

Opening/reopening orders the same child-owned editor just above its owner
without calling makeKeyAndOrderFront or activating the bridge. Clicking the
editor lets AppKit activate it normally. Plugin-requested show retains the
last owner number; reopen updates it. `PluginWindows::appActivated()` reorders
open Cocoa bridged editors when the DAW regains focus, skipping instances
already closed or replaced. The GUI main-loop hook is routed through port-gui.
Other applications can cover these normal-level windows.

## Validation and limits

Hermetic host tests cover owner-number normalization, wire round trips and
out-of-range rejection. A display-opt-in macOS test checks normal level and
cross-process relative order using CGWindowListCopyWindowInfo, open/reopen
without changing the frontmost application, and owner replacement while
retaining the editor window. It requires an interactive desktop and stays
behind SUPPLE_GUI_DISPLAY_TEST=1; headless CI does not establish z-order,
activation or native input behavior. No evidence yet requires the fallback
of DAW-driven hiding on deactivation. Placement/focus persistence and MIDI-learn
focus tracking retain the limits documented in ADR-0040.

API: [NSWindow relative ordering](https://developer.apple.com/documentation/appkit/nswindow/order(_:relativeto:)),
[window list metadata](https://developer.apple.com/documentation/coregraphics/cgwindowlistcopywindowinfo(_:_:)).
