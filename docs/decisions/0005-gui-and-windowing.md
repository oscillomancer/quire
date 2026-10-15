# ADR-0005: GUI toolkit and windowing (XWayland)

**Status:** Accepted, 2026-09-26. Evidence: `docs/research/gui-stack.md`,
`docs/research/gui-spike.md` (spike code in `tools/spikes/gui-shell/`).

**Editor amendment, 2026-10-14:** [ADR-0040](0040-native-plugin-editors.md)
adds native local editors, Windows bridged embedding and Cocoa bridge-owned
floating windows.

**Native windowing amendment, 2026-10-14:** [ADR-0039](0039-native-gui.md)
extends the shell to Cocoa/Windows, with platform scaling, primary shortcuts,
native waitables and platform-specific rendering tests; Linux behavior stays.

## Context

Third-party plugin GUIs on Linux expect an X11 parent window
(`CLAP_WINDOW_API_X11`); a native Wayland surface has no XID to give them.
The machine runs Hyprland 0.56 with XWayland 24.1. Qt and GTK are ruled out by
the license policy. The DAW needs REAPER-class responsiveness, its own visual
identity, and ideally shared rendering technology with the stock plugins' editors
(Pugl + NanoVG).

The spike on this machine showed:
- SDL3 3.4.16 with the `x11` driver forced gives an XWayland window whose XID
  (`SDL_PROP_WINDOW_X11_WINDOW_NUMBER`) parents X11 child containers; the
  template's Starter editor embedded, rendered, resized twice and survived 3
  create/destroy cycles with the host driving `clap.timer-support`.
- GL: requested 3.3 core, got 4.6 core on the Arc B580 (Mesa 26.2.3).
- Event-driven idle cost 0.0025 CPU-seconds over 5 s.
- Naive NanoVG drawing of 64 × 8 waveforms × 1500 points per frame cost
  ~81 ms CPU per frame: NanoVG tessellates paths on the CPU, so bulk data
  must not go through it.
- Hyprland (tiling) overrides requested window sizes asynchronously; hosts
  must lay out from observed configure events, never from requested sizes.

## Decision

1. **Windowing: SDL3, X11 driver forced** (`SDL_HINT_VIDEO_DRIVER=x11`) for
   all DAW windows. The main window and all floating windows run through
   XWayland. SDL3 (Zlib, system library) also supplies input, text input/IME,
   clipboard, file drag-and-drop, file dialogs (portal) and display scale.
2. **Plugin GUIs** embed as X11 children of per-plugin container windows
   parented to SDL windows' XIDs; floating plugin windows are SDL X11
   top-levels with the same container. Bridged plugins (separate processes)
   embed the same way — X11 XIDs are global. The host implements
   `clap.timer-support` and `clap.gui` resize negotiation driven by observed
   geometry.
3. **Rendering:** one OpenGL 3.3 core context per window.
   - **NanoVG (GL3 backend)** draws widgets, text and chrome — the same
     renderer family as the plugin editors, so the look and type quality can
     be shared.
   - **Bulk renderers** in plain GL draw data-dense layers: waveforms from
     multi-resolution min/max peak pyramids (one quad/line pair per pixel
     column, uploaded as vertex buffers and cached per zoom level), MIDI
     notes as instanced rectangles, meters and spectra as buffers. These are
     the only way to meet the frame budget at every zoom level.
4. **Toolkit:** Quire's own retained-mode widget tree (`daw/gui/ui`): layout,
   focus, keyboard navigation, damage tracking (redraw only dirty regions/
   layers), virtualized lists and track rows (only visible rows exist),
   themes as token sets (like the template's `Theme.h` contract, extended).
   Nothing in the UI touches the audio thread; it reads model versions and
   telemetry rings.
5. **Performance gates for Phase 5:** p95 frame ≤ 8 ms CPU at 1080p with a
   64-track session in the arrange view at any zoom; idle CPU ≈ 0; cold
   start to interactive window < 300 ms on this machine. Tracked in
   `docs/benchmarks/`.
6. **Pugl** stays inside the plugins (the template's stack). It is not used
   for the DAW shell: it lacks IME/text composition, file dialogs and file
   drag-and-drop, which a DAW needs and SDL3 already has.

## Consequences

The DAW is an X11 client on Wayland desktops, like REAPER and Bitwig on
Linux. A native Wayland main window is possible later only if plugin GUIs
float in separate X11 windows (an option, not the default). The rendering
split (NanoVG for chrome, GL batches for data) must be designed in from the
start of Phase 5. Open items carried to Phase 5: keyboard focus handoff into
and out of embedded plugin editors, text input delivery under XWayland,
fractional scaling, and reliable window capture for UI tests (XGetImage of
the GL parent was unreliable after resize; use GL readback instead).

## Alternatives considered

- **Pugl as the DAW shell:** matches the plugins exactly, but missing app
  features listed above.
- **GLFW:** smaller than SDL3 with the same X11 constraint and fewer app
  features.
- **Dear ImGui:** fast to build tools with, but immediate-mode idle costs,
  weak text editing/accessibility, and hard to give a distinct, polished
  identity; may still be used for internal debug overlays.
- **RmlUi / web views:** heavier, less control over frame cost, and a second
  UI language.
- **Native Wayland main window:** cannot parent X11 plugin GUIs.
