# Nave integration

Template instantiated with Reverb/reverb/REVERB code identity, CLAP
`com.suppleaudio.reverb`, AU `QRvb`, state `QRV1`; display identity lives in
`cmake/Identity.cmake`. Mono/stereo effect, version 0.1.0, zero latency.

The monorepo and ADR-0011 retain the template MIT license; its GPL product
conversion checklist does not apply. No template source was modified.

## Product-owned integration

- The SDK-free engine owns the core-params table, flat patch codec and DSP;
  the wrapper forwards params/state/semantic operations to plugin-support.
  CLAP value positions remain normalized; patches and editor values are
  physical. State framing is canonical JSON and loading is transactional.
- The isolated ExternalProject builds the same engine under a distinct
  `supple/reverb-engine` binary directory. Dependencies use the monorepo's
  vendored, pinned sources; C++20, warning/RT sanitizer flags and CLAP-only
  defaults follow the compressor's build seam. No other plugin is linked.
- Tail is a product extension, with audio-thread notifications and atomic
  main-thread deferral. There are no latency-changing controls or sidechain.
- The editor, palette, drawing widgets, telemetry frame and preview tool are
  product-owned. The telemetry Ring, window, renderer, Gui, host contract,
  wrapper patches and Linux embedding probe retain template infrastructure
  apart from namespace/identity. The general CLAP probe queries Output in
  place of the template's removed output meter. NanoVG includes are SYSTEM.
- Linux CLAP/VST3 targets link their explicit export maps with LINK_DEPENDS;
  exports-clap checks that only clap_entry is public. The self-handle patch
  file is carried, but its mandatory presence gate is deferred: the reverse
  apply probe fails in this base's vendored fsutil.cpp. The orchestrator owns
  applying that shared patch on the integration branch.
- All template core/editor tests are adapted to these parameters. Added
  transactional/concurrent semantic and tail-thread tests. The unchanged
  host contract rejects bare ASCII `-inf`; CLAP text retains the canonical
  off label (`-inf dB`) emitted by core-params, while physical patch text can
  still use core-params' off aliases. Finite state stores -60, never infinity.
- Preview scenarios default and hall render the actual editor and real engine
  telemetry in a fixed-size Linux parent. Previews prove drawing; the separate
  Linux embedding probe covers resize, show/hide, reopen and teardown.
- Structure, interpolation, diffuser cap and measurement decisions are in
  stock-effects.md §7. The format gate checks product-owned files, preserving
  copied infrastructure. No dependencies were added.

The dense RT60/density measurements are preserved in the opt-in
reverb-engine.measure target (EXCLUDE_FROM_ALL, never registered with CTest).
Routine tests use representative short IRs and aggregated assertions; no
raised timeout or production DSP change. See VERIFICATION.md for timings.
